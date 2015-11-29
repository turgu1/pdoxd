# the compiler: gcc for C program, define as g++ for C++
CC = gcc

# compiler flags:
#  -g    adds debugging information to the executable file
#  -Wall turns on most, but not all, compiler warnings
CFLAGS  = -Wall -lrt
LIBS = -l paho-mqtt3a

# the build target executable:
TARGET = pdoxd

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LIBS)

clean:
	$(RM) $(TARGET)

update: $(TARGET)
	service pdoxd stop
	cp pdoxd /opt/pdoxd/sbin
	service pdoxd start

install: $(TARGET)
	cp $(TARGET) /opt/$(TARGET)/sbin
	cp $(TARGET).conf /opt/$(TARGET)/etc
	cp $(TARGET).sh /etc/init.d/$(TARGET)
	chmod 755 /etc/init.d/$(TARGET)
	updated-rc.d -f $(TARGET).sh defaults
	cp tools/$(TARGET).logrotate /etc/logrotate.d/$(TARGET)
	cp tools/$(TARGET).conf.rsyslogd /etc/rsyslog.d/$(TARGET).conf

