
DEPENDENCIES := -lpthread
UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
	DEPENDENCIES += -lobjc -framework IOKit -framework CoreFoundation
else
	DEPENDENCIES += -lrt
endif

CFLAGS = -Wall -Wextra

DYNAMIC_LINK_OPTIONS := -Wl,-rpath /usr/local/lib

APP = sdcdmux

all: $(APP)

$(APP): $(APP).c
	$(CC) $(APP).c -o $(APP) -lftd2xx $(CFLAGS) $(DEPENDENCIES)
	
clean:
	-rm -f *.o ; rm $(STATIC_APP); rm $(DYNAMIC_APP)
