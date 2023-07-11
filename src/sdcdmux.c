/**
	SDCDMUX V0.1 for eMMC-Version-PCB

		Copyright (c) 2023 Hiroshi Nakajima. All rights reserved.

**/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "ftd2xx.h"

#define MAX_CAPTURE_WAIT	100000
#define MIN_COMMAND_BUF_LEN	512
#define LEADER_CODE_LEN 	692	// 9ms=692
#define MINIMUM_IFR_LEN		40	// 562uS=1T
#define TMP_BUF_SIZE		8192
#define PREAMBLE_BIT_SIZE	LEADER_CODE_LEN

#define START_LOW_THRESHOLD_LEN	200	// > 2.6ms
#define CAPTURE_BUFFER_SIZE	512000
#define NUM_TX_IFR_CMD		1
#define DEFAULT_OFF_TIME	10
#define DEFAULT_ON_TIME		60
#define CLOCK_RATE_FOR_IFR	15200 // 13uS/8bit 2byte(38kHz) duty=1/2

#define TARGET_TB   0
#define TARGET_HOST 1
#define TARGET_BIT  0

#define POWER_ON    1
#define POWER_OFF   0
#define POWER_BIT   1  

#define ACTION_SELECT       0
#define ACTION_CMD        	1
#define ACTION_POWER_LOOP   2
#define ACTION_GET_STATUS   3
#define ACTION_CAPTURE		4

#define NO_ERR	0
#define GNL_ERR	1

static int debug = 0;
static int running = 0;

extern char *optarg;

static void __signal_handler(__attribute__ ((unused)) int dummy);
static char *get_local_time(char *buf, int len);
static void power_loop_test(int on_time, int off_time, int n_tx, int dumpbit);
static void wating(int wait_time);
static int command_tx(char *cmdname, int n_tx, int dumpbit);
static int set_ind(UCHAR val);

static void __signal_handler(__attribute__ ((unused)) int dummy)
{
	running=0;
	return;
}

static char *get_local_time(char *buf, int len){
    time_t t = time(NULL);
    struct tm *local_time = localtime(&t);
    strftime(buf, len, "%Y/%m/%d %H:%M:%S %a", local_time);
    return buf;
}

static void wating(int wait_time){
    int count = 0;
    while(running){
        if(count++ >= wait_time){
            return;
        }
        sleep(1);
    }
}

static FT_STATUS ftdi_open(FT_HANDLE *pfth){
	FT_STATUS	fts = FT_OK;

	fts = FT_Open(0, pfth);
	if (fts != FT_OK) 
	{
		printf("FT_Open failed (error %d).\n", (int)fts);
		printf("You should remove old ftdi_sio and usbserial driver. Please do \"./release_mod.sh\".\n");
	}
	return fts;
}

static void power_loop_test(int on_time, int off_time, int n_tx, int dumpbit){
	char buf[128];
    unsigned int count = 0;

    while(running){
        count++;
        printf("POWER ON : No.%d  %s\n", count, get_local_time(buf, sizeof(buf)));
		command_tx("on", n_tx, dumpbit);
		set_ind(0x0);
        wating(on_time);
        printf("POWER OFF: No.%d  %s\n", count, get_local_time(buf, sizeof(buf)));
		command_tx("off", n_tx, dumpbit);
		set_ind(0x8);
        wating(off_time);
    }
	set_ind(0xC);
}

void bit_show(UCHAR *buf, unsigned int len, int shiftbit)
{
	unsigned int i;
	UCHAR dat;

	for(i=0;i<len;i++){
		dat = (*(buf+i) >> shiftbit) & 0x01;
		printf("%1d", dat);
	}
	printf("\n");
	fflush(stdout);
}

void ifr_lowpass_filter(UCHAR *buf, DWORD len, int first)
{
	static int pbit, lowcont, low_start_ofs;
	DWORD i;
	int bit, pad;
	if(first){
		pbit = 1;
		lowcont = 0;
		low_start_ofs = -1;
		return;
	}
	for(i=0;i<len;i++){
		bit = *(buf+i) & 0x01;
		if(bit == 1){
			if(low_start_ofs >= 0 && lowcont < MINIMUM_IFR_LEN){
				for(pad=0;pad<lowcont;pad++){
					*(buf+low_start_ofs+pad) = 1;
				}
			}
			low_start_ofs = -1;
			pbit = bit;
			lowcont = 0;
		}
		else {
			// bit = 0
			if(low_start_ofs < 0 && pbit == 1){
				low_start_ofs = i;
			}
			lowcont++;
			pbit = bit;
		}
	}
}


DWORD ifr_filter(UCHAR *buf, DWORD len, int first)
{
	static int pbit, valid, highcont, lowcont;
	DWORD outlen = 0, i;
	int bit, outvalid;
	UCHAR *out;
	out = buf;
	if(first){
		pbit = 1;
		valid = 0;
		highcont = 0;
		lowcont = 0;
		return 0;
	}
	outvalid = 0;
	for(i=0;i<len;i++){
		bit = *(buf+i) & 0x01;
//		printf("i=%d bit=%d pbit=%d valid=%d highcont=%d outlen=%d\n", i, bit, pbit, valid, highcont, outlen);
		if(valid == 0){
		 	if(bit == 1){
				pbit = 1;
				lowcont = 0;
				continue;
			}
			else {
				// bit == 0
				if(pbit == 0 && bit == 0){
					lowcont++;
				}
				*(out+outlen++) = bit;
				if(lowcont > START_LOW_THRESHOLD_LEN){
					valid = 1;
					outvalid = 1;
				}
				pbit = bit;
			}
		}
		else {
			// valid = 1
			if(pbit == 1 && bit == 1)
				highcont++;
			if(bit == 0){
				highcont = 0;
			}
			*(out+outlen++) = pbit = bit;
			if(highcont > LEADER_CODE_LEN){
				valid = 0;
				highcont = 0;
			}
		}
	}
	if (outvalid == 0){
		return 0;
	}
	return outlen;
}

static int set_ind(UCHAR val)
{
	UCHAR pinStatus;
	FT_STATUS 	fts;
	FT_HANDLE	fth;

	fts = ftdi_open(&fth);
	if (fts != FT_OK){
		return fts;
	}

	fts = FT_GetBitMode(fth, &pinStatus);
	val = (pinStatus & 0x01) | (val & 0x0C);
	fts = FT_SetBitMode(fth, 
						0xF0 | (val & 0x0F), // C9,C8,C6,C5
						FT_BITMODE_CBUS_BITBANG);
	(void)FT_Close(fth);
	return fts;
}


int ifr_cmd_capture(FT_HANDLE fth, UCHAR *buf, int maxbuf, DWORD *prcvbuf)
{
	UCHAR tmpbuf[TMP_BUF_SIZE];
	FT_STATUS 	fts;
	DWORD bytesRead = 0, outlen = 0, outofs = 0, wait, room;

	if (maxbuf < MIN_COMMAND_BUF_LEN){
		printf("Invalid buffer length %d\n", maxbuf);
		return FT_INVALID_PARAMETER;
	}
	fts = FT_SetBitMode(fth, 
	                         0x00, // ADBUS1 is TX, ADBUS0 is RX
	                         FT_BITMODE_ASYNC_BITBANG);
	if (fts != FT_OK) 
	{
		printf("FT_SetBitMode failed (error %d).\n", (int)fts);
		return fts;
	}

	// discard data which a device has them in buffer before capturing //
	outlen = 1;
	ifr_filter(NULL, 0, 1);
	while (outlen != 0){
		fts = FT_Read(fth, tmpbuf, TMP_BUF_SIZE, &bytesRead);
		if(fts != FT_OK){
			return fts;
		}
		outlen = ifr_filter(tmpbuf, bytesRead, 0);
		// printf("Rcv Byte %d for discard outlen=%d \n", bytesRead, outlen);		
	}

	printf("Caputure Start.. \n");
	ifr_filter(NULL, 0, 1);
	ifr_lowpass_filter(NULL, 0, 1);
	outofs = PREAMBLE_BIT_SIZE;
	room = maxbuf - PREAMBLE_BIT_SIZE;
	memset(buf, 0x01, outofs); 
	for(wait=0;wait < MAX_CAPTURE_WAIT;wait++){
		fts = FT_Read(fth, tmpbuf, TMP_BUF_SIZE, &bytesRead);
		if(fts != FT_OK){
			return fts;
		}
		ifr_lowpass_filter(tmpbuf, bytesRead, 0);
//		bit_show(tmpbuf, bytesRead, 0);
		outlen = ifr_filter(tmpbuf, bytesRead, 0);
//		printf("Rcv Byte %d room=%d outlen=%d\n", bytesRead, room, outlen);
//		bit_show(tmpbuf, outlen, 0);
		if(outlen == 0){
			if(outofs > PREAMBLE_BIT_SIZE){
				*prcvbuf = outofs;
				return FT_OK;
			}
			continue;			
		}
		if(room < outlen){
			printf("overflow buffer room=%d outlen=%d\n", room, outlen);
			outlen = room;
		}
		memcpy(buf+outofs, tmpbuf, outlen);
		outofs += outlen;
		room -= outlen;
	}
	printf("expired wait time.\n");
	*prcvbuf = outofs;
	return FT_OK;
}

int encode_ifr(UCHAR *buf, DWORD cmdlen){
	DWORD i;
	UCHAR pbit = 1, bit;

	for(i=0;i<cmdlen;i++){
		bit = *(buf+i) & 0x01;
		if(pbit == 0 && bit == 0)
			bit = 1;
		*(buf+i) = (bit << 1);
		pbit = bit;
	}
	*(buf+cmdlen-1) = 0;
	return FT_OK;
}

DWORD tx_ifr_cmd(FT_HANDLE fth, UCHAR *buf, DWORD cmdlen){
	DWORD bytesWritten;
	FT_STATUS ftStatus;

	ftStatus = FT_SetBitMode(fth, 
	                         0xFF, // ADBUS1 is TX, ADBUS0 is RX
	                         FT_BITMODE_ASYNC_BITBANG);
	if (ftStatus != FT_OK) 
	{
		printf("FT_SetBitMode failed (error %d).\n", (int)ftStatus);
		return ftStatus;
	}
	ftStatus = FT_Write(fth, buf, cmdlen, &bytesWritten);
	if (ftStatus != FT_OK)
	{
		printf("FT_Write failed (error %d).\n", (int)ftStatus);
	}
	return bytesWritten;
}

int save_ifr_cmd(UCHAR *buf, DWORD cmdlen, char *cmdname){
	FILE *fop;
	int wlen;
	char filename[64];

	if(strlen(cmdname) > 60){
		fprintf(stderr, "cmdname length should be less than 60.\n");
		return GNL_ERR;
	}
	sprintf(filename, "ifr-%s", cmdname);
	fop = fopen(filename, "wb+");
	if(fop == NULL){
		fprintf(stderr, "Could not create the file.\n");
		return GNL_ERR;
	}
	wlen = fwrite(buf, 1, cmdlen, fop);
	if(wlen < 0){
		return GNL_ERR;
	}
	fclose(fop);
	return NO_ERR;
}

int load_ifr_cmd(UCHAR *buf, int maxbuf, char *cmdname){
	FILE *fp;
	int cmdlen;
	char filename[64];

	if(strlen(cmdname) > 60){
		fprintf(stderr, "cmdname length should be less than 60.\n");
		return GNL_ERR;
	}
	sprintf(filename, "ifr-%s", cmdname);
	fp = fopen(filename, "rb");
	if(fp == NULL){
		fprintf(stderr, "Could not open the file.\n");
		return GNL_ERR;
	}
	cmdlen = fread(buf, 1, maxbuf, fp);
	if(cmdlen < 0){
		return GNL_ERR;
	}
	fclose(fp);
	return cmdlen;
}


int command_capture(char *cmdname, int dumpbit){
	FT_STATUS	fts = FT_OK;
	FT_HANDLE	fth;

	fts = ftdi_open(&fth);
	if (fts != FT_OK){
		return fts;
	}

	fts = FT_SetBaudRate(fth, CLOCK_RATE_FOR_IFR);
	if (fts != FT_OK) 
	{
		printf("FT_SetBaudRate failed (error %d).\n", (int)fts);
		goto exit;
	}

	DWORD cmdlen;
	UCHAR buf[CAPTURE_BUFFER_SIZE];
	fts = ifr_cmd_capture(fth, buf, CAPTURE_BUFFER_SIZE, &cmdlen);
	if(fts != FT_OK){
		goto exit;
	}
	if(dumpbit){
		bit_show(buf, cmdlen, 0);
	}
	// printf("\nEncording..\n");
	encode_ifr(buf, cmdlen);
	// bit_show(buf, cmdlen, 1);
	// printf("\nSave command\n");
	save_ifr_cmd(buf, cmdlen, cmdname);
	if(cmdlen > 0)
		fts = FT_OK;
	printf("Recieve IFR data (%dB)\n", cmdlen);

exit:

	(void)FT_Close(fth);
	return fts;
}

int command_tx(char *cmdname, int n_tx, int dumpbit){
	FT_STATUS	fts = FT_OK;
	FT_HANDLE	fth;

	fts = ftdi_open(&fth);
	if (fts != FT_OK){
		return fts;
	}

	fts = FT_SetBaudRate(fth, CLOCK_RATE_FOR_IFR);
	if (fts != FT_OK) 
	{
		printf("FT_SetBaudRate failed (error %d).\n", (int)fts);
		goto exit;
	}
	DWORD cmdlen;
	UCHAR buf[CAPTURE_BUFFER_SIZE];
	cmdlen = load_ifr_cmd(buf, CAPTURE_BUFFER_SIZE, cmdname);
	if(dumpbit){
		bit_show(buf, cmdlen, 1);
	}
	for(int j=0;j<n_tx;j++){
		tx_ifr_cmd(fth, buf, cmdlen);
	}
	printf("Tx Command %s (%d)\n", cmdname, cmdlen);

exit:

	(void)FT_Close(fth);
	return fts;
}

int write_to_tbctl(UCHAR val){
	FT_STATUS	fts = FT_OK;
	FT_HANDLE	fth;

	fts = ftdi_open(&fth);
	if (fts != FT_OK){
		return fts;
	}

	fts = FT_SetBitMode(fth, 
	                         0xF0 | (val & 0x0F), // C9,C8,C6,C5
	                         FT_BITMODE_CBUS_BITBANG);
	if (fts != FT_OK)
	{
		printf("FT_SetBitMode failed (error %d).\n", (int)fts);
	}
	(void)FT_Close(fth);
	return 0;
}

int display_status(void){
	FT_STATUS	fts = FT_OK;
	FT_HANDLE	fth;
	UCHAR pinStatus;

	fts = ftdi_open(&fth);
	if (fts != FT_OK){
		return fts;
	}

	fts = FT_GetBitMode(fth, &pinStatus);
	if (fts != FT_OK) 
	{
		printf("FT_GetBitMode failed (error %d).\n", (int)fts);
	}
	printf("Memory Device is connected to : %s.\n", (pinStatus & 0x01) ? "HOST" : "TARGET");
	(void)FT_Close(fth);
	return fts;
}

void usage(void){
	fprintf(stderr, "USB2SDMUX VERSION 0.1\n");
	fprintf(stderr, "Usage: usb2sdmux [OPTION]...\n");
	fprintf(stderr, "          -c <cmdname>             CAPTURE IFR COMMAND\n");
	fprintf(stderr, "          -x <cmdname>             TRANSMIT IFR COMMAND\n");
	fprintf(stderr, "          -n <tx count>            NUMBER OF TRANSMITING IFR COMMAND (default %d)\n", NUM_TX_IFR_CMD);
	fprintf(stderr, "          -s <host | target>       SELECT CONNECTING SD CARD\n");
	fprintf(stderr, "          -l                       START POWER ON/OFF CYCLE(using on & off command)\n");
	fprintf(stderr, "          -o <ontime>              POWER ON/OFF CYCLE ON TIME (default %dsec)\n", DEFAULT_ON_TIME);
	fprintf(stderr, "          -f <offtime>             POWER ON/OFF CYCLE OFF TIME (default %dsec)\n", DEFAULT_OFF_TIME);
	fprintf(stderr, "          -h                       SHOW HELP\n");
	fprintf(stderr, "          -v                       SHOW IFR COMMAND BIT\n");
	exit(1);
}


int main(int argc, char *argv[])
{
	int	status, n_tx = NUM_TX_IFR_CMD;
	int target, action, dumpbit = 0;
    unsigned char val;
  	int on_time = DEFAULT_ON_TIME, off_time = DEFAULT_OFF_TIME;
	char opt, *cmdname;

	action = ACTION_GET_STATUS;

	while(( opt = getopt(argc, argv, "vhlc:x:s:o:f:n:")) != -1){
		switch(opt){
			case 'c':
				action = ACTION_CAPTURE;
				cmdname = optarg;
				break;
			case 'x':
				action = ACTION_CMD;
				cmdname = optarg;
				break;
			case 's':
				action = ACTION_SELECT;
				cmdname = optarg;
				break;
			case 'l':
				action = ACTION_POWER_LOOP;
				break;
			case 'n':
				n_tx = atoi(optarg);
				if(n_tx == 0){
					usage();
					exit(1);
				}
				break;
			case 'o':
				on_time = atoi(optarg);
				if(on_time == 0){
					usage();
					exit(1);
				}
				break;
			case 'f':
				off_time = atoi(optarg);
				if(off_time == 0){
					usage();
					exit(1);
				}
				break;
			case 'v':
				dumpbit = 1;
				break;
			case 'h':
				usage();
				break;
			default:
				usage();
				exit(1);
				break;
		}
	}

    switch (action){
		case ACTION_CAPTURE:
			//for(int j=0;j<2;j++){
			set_ind(0);
			status = command_capture(cmdname, dumpbit);
			//}
			if(status == FT_OK){
				printf("Caputured command for %s\n", cmdname);
			}
			set_ind(0x0C);
			break;
        case ACTION_SELECT:
			if(cmdname[0] == 'h'){
				target = TARGET_HOST;
			}
			else if(cmdname[0] == 't'){
				target = TARGET_TB;
			}
			else {
				usage();
				exit(1);
			}
            if (debug) printf("ACTION_SELECT %d\n", target);
			val = (unsigned char)target << TARGET_BIT;
			write_to_tbctl(val);
			display_status();
            break;
        case ACTION_CMD:
			command_tx(cmdname, n_tx, dumpbit);
            break;
        case ACTION_GET_STATUS:
            display_status();
            break;
        case ACTION_POWER_LOOP:
            signal(SIGINT, __signal_handler);
        	running=1;
            power_loop_test(on_time, off_time, n_tx, dumpbit);
            break;
        default:
            break;
    }

	return 0;
}
