#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/udp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
//#include <time.h>
//#include <errno.h>

#include "MQTTClient.h"

int debugLevel;

char tty[80];      // Serial port device name
int baud;          // Baud rate

char netIf[80];    // Network Interface Name (eth0)

char hosts[5][80]; // Server address to send info to
int hostsCnt;
unsigned int port; // UDP Port to send to

char mqtt_address[121];
char mqtt_server[80];
char mqtt_topic[80];
unsigned int mqtt_port; // MQTT Port to send to

char * mqtt_client_id = "pdox_server";

char data[256];    // Data read from tty and sent to udp port
char buff[256];
int buffIdx;
int buffCnt;

int serialPort;    // Serial port
int socks[5];      // UDP sockets
int socksCnt;

struct termios serialConfig;   // Serial port config options

struct sockaddr_in myaddr;     // socket binding data for this machine
struct sockaddr_in toaddrs[5]; // socket binding data for the host

// ----- Timer Signal Handling -----
/*
timer_t timer;

int startTimer()
{
  struct itimerspec its;

  its.it_value.tv_sec = 60;
  its.it_value.tv_nsec = 0;
  its.it_interval.tv_sec = 60;
  its.it_interval.tv_nsec = 0;

  if (timer_settime(timer, 0, &its, NULL)) {
    Log(ERR, "startTimer unable to initialise timer: %s", strerror(errno));
    return 1;
  }
  return 0;
}

void stopTimer()
{
  signal(SIGALRM, SIG_IGN);
}

void timeHandler(int sig)
{
  printf("Caught signal %d\n", sig);
  signal(sig, SIG_IGN);
  //startTimer();
  signal(sig, timeHandler);
}

int initTimer()
{
  int rc;

  if (signal(SIGALRM, timeHandler)) {
    Log(ERR, "initTimer unable to set signal SIGALRM: %s", strerror(errno));
    return 1;
  }

  if (timer_create(CLOCK_MONOTONIC, NULL, &timer)) {
    Log(ERR, "initTimer unable to create a timer: %s", strerror(errno));
    return 1;
  }

  return startTimer(); 
}
*/

// ----- Daemon Control -----

static void skeletonDaemon()
{
    FILE *f;
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0) {
        if ((f = fopen("/opt/pdoxd/var/run/pdoxd.pid", "w")) != NULL) {
          fprintf(f, "%d\n", pid);
          fclose(f);
        }
        exit(EXIT_SUCCESS);
    }

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir("/");

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>0; x--)
    {
        close (x);
    }
}

// ------ Logging tools -----

#define ERR   1
#define INFO  2
#define DATA  3
#define DEBUG 4

void openLog()
{
  openlog("pdox", LOG_PID | LOG_PERROR, LOG_LOCAL7);
  setlogmask(
    LOG_MASK(LOG_INFO) | 
    LOG_MASK(LOG_NOTICE) | 
    LOG_MASK(LOG_ERR) | 
    LOG_MASK(LOG_DEBUG));
}

void closeLog()
{
  closelog();
}

void Log(int kind, char *fmt, ...)
{
  char str[256];
  char *skind;
  int lkind;
  va_list ap;

  va_start(ap, fmt);

  switch (kind) {
    case ERR:
      skind = "ERROR";
      lkind = LOG_ERR;
      break;
    case INFO:
      skind = "INFO";
      lkind = LOG_NOTICE;
      break;
    case DATA:
      skind = "DATA";
      lkind = LOG_INFO;
      break;
    case DEBUG:
      skind = "DEBUG";
      lkind = LOG_DEBUG;
      break;
  }

  if (debugLevel) {
    vsprintf(str, fmt, ap);

    fprintf(stderr, "%s: %s\n", skind, str);
  }
  else {
    vsyslog(lkind, fmt, ap);
  }
}

// ----- Configuration File Reader -----

int readHosts(char *str)
{
  int i;
  char *ptr;

  if (debugLevel) Log(DEBUG, "readHosts: Data to parse: %s", str);

  i = 0;
  while (1) {
    ptr = str;
    while (*str && (*str != ',')) str++;
    *str++ = 0;
    if (strlen(ptr) == 0) break;
    strcpy(hosts[i], ptr);
    if (++i >= 5) break;
  }
  hostsCnt = i;

  if (debugLevel) Log(DEBUG, "readHosts: Hosts Count: %d", i);

  return i;
}

int readConfig(char *filename)
{
  FILE *fi;
  char *s;
  char *param;
  char *value;
  int i;
  char str[120];

  // Default values
  strcpy(hosts[0], "192.168.1.10");
  strcpy(tty,      "/dev/ttyAMA0");
  strcpy(netIf,    "eth0");
  port = 3001;
  baud = 2400;
  hostsCnt = 1;
  mqtt_port = 1883;
  strcpy(mqtt_server, "mosquitto");
  strcpy(mqtt_topic, "paradox");

  if ((fi = fopen(filename, "r")) == NULL) {
    Log(ERR, "readConfig: Unable to open config file");
    return 0;
  }

  while (fgets(buff, 255, fi)) {

    s = buff;
    while (*s == ' ') s++;
    if ((*s != '#') && (*s != '\n') && (*s != '\r')) {
      param = s;
      while (isalpha(*s) || (*s == '_')) s += 1;
      *s++ = 0;
      while (*s == ' ') s++;
      if (*s == ':') s++;
      while (*s == ' ') s++;
      value = s;
      while ((*s != '\0') && (*s != ' ') && (*s != '\n') && (*s != '\r')) s++;
      *s = 0;

      // Log(INFO, "Reading config file: %s: %s", param, value);

      if (strcmp(param, "tty") == 0) strcpy(tty, value);
      else if (strcmp(param, "net_if") == 0) strcpy(netIf, value);
      else if (strcmp(param, "port") == 0) port = atoi(value);
      else if (strcmp(param, "baud") == 0) baud = atoi(value);
      else if (strcmp(param, "hosts") == 0) readHosts(value);
      else if (strcmp(param, "mqtt_port") == 0) mqtt_port = atoi(value);
      else if (strcmp(param, "mqtt_server") == 0) strcpy(mqtt_server, value);
      else if (strcmp(param, "mqtt_topic") == 0) strcpy(mqtt_topic, value);
      else Log(ERR, "Unknown config parameter: %s", param);
    }
  }

  snprintf(mqtt_address, 120, "tcp://%s:%d", mqtt_server, mqtt_port);

  Log(INFO, "readConfig: Configuration:");
  strcpy(str, "readConfig: Host(s): ");
  for (i = 0; i < hostsCnt; i++) {
    strcat(str, hosts[i]);
    if ((i+1) < hostsCnt) strcat(str, ", ");
  }
  Log(INFO, str);
  Log(INFO, "readConfig: Port: %d",          port);
  Log(INFO, "readConfig: TTY: %s, baud: %d", tty, baud);
  Log(INFO, "readConfig: MQTT address: %s",  mqtt_address);
  Log(INFO, "readConfig: MQTT topic: %s",    mqtt_topic);

  fclose(fi);

  if (hostsCnt <= 0) {
    Log(ERR, "At least one host must be identified");
    return 1;
  }

  return 0;
}

// ----- MQTT processing -----

MQTTClient mqtt_client;

// ---- connectMQTT() -----

int connectMQTT()
{
  MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
  int rc;

  conn_opts.keepAliveInterval = 20;
  conn_opts.cleansession = 0;
  conn_opts.reliable = 0;

  if ((rc = MQTTClient_connect(mqtt_client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
    Log(ERR, "openMQTT: Failed to connect to %s, return code %d\n", mqtt_address, rc);
    return 1;
  }

  return 0;
}

// ----- connectionLostMQTT() -----

void connectionLostMQTT(void *context, char *cause)
{
  while (connectMQTT()) sleep(10);
}

// ----- openMQTT() -----

int openMQTT()
{
  MQTTClient_create(&mqtt_client, mqtt_address, mqtt_client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);

  MQTTClient_setCallbacks(mqtt_client, NULL, connectionLostMQTT, NULL, NULL);

  return connectMQTT();
}

// ----- sendMQTT() -----

int sendMQTT()
{
  MQTTClient_message pubmsg = MQTTClient_message_initializer;
  MQTTClient_deliveryToken token;
  int rc;

  pubmsg.payload    = data;
  pubmsg.payloadlen = strlen(data);
  pubmsg.qos        = 1;
  pubmsg.retained   = 0;

  rc = MQTTClient_publishMessage(mqtt_client, mqtt_topic, &pubmsg, &token);

  if (rc != 0) {
    Log(ERR, "Error code from MQTTClient_publishMessage: %d", rc);
  }

  return 0;
}

// ----- UDP Ports Management -----

int openUDP()
{
  struct hostent *hp;
  int i;
  
  for (i = 0; i < hostsCnt; i++) {
    socks[i] = socket(AF_INET, SOCK_DGRAM, 0);
    if (socks[i] < 0) {
      Log(ERR, "openUDP: Unable to create UDP socket");
      return 1;
    }

    memset((char *) &myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(0);

    if (bind(socks[i], (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
      Log(ERR, "openUDP: Unable to bind UDP socket to port");
      return 1;
    }

    if (!(hp = gethostbyname(hosts[i]))) {
      Log(ERR, "openUDP: Could not obtain address");
      return 1;
    }

    memset((char *) &toaddrs[i], 0, sizeof(toaddrs[i]));
    toaddrs[i].sin_family = AF_INET;
    toaddrs[i].sin_port = htons(port);
    memcpy((void *) &toaddrs[i].sin_addr, hp->h_addr_list[0], hp->h_length);

    if (strcmp("255.255.255.255", hosts[i]) == 0) {
      if (debugLevel) Log(DEBUG, "Broadcasting");
      int enable = 1;
      if (setsockopt(socks[i], SOL_SOCKET, SO_BROADCAST, &enable, sizeof(int))) {
        Log(ERR, "openUDP: Unable to set broadcast mode on socket");
        return 1;
      }
    }
  }
  socksCnt = hostsCnt;
  return 0;
}

void filterStr(char *outStr, char *inStr, int max) 
{
  char tmp[10];
  char *s;

  while (*inStr && (max > 0)) {
    if ((*inStr >= ' ') && (*inStr <= '~')) {
      *outStr++ = *inStr++;
      max--;
    } 
    else {
      sprintf(tmp, "<%d>", *inStr++);
      if (max < strlen(tmp)) break;
      s = tmp;
      while (*s) *outStr++ = *s++;
    }
  }
  *outStr = 0;
}
 
void writeUDP()
{
  char str[201];
  char *ptr;
  int i;

  // if ((strlen(data) > 18) && isdigit(data[0]) && isdigit(data[1]) && isdigit(data[2]) && isdigit(data[3])) {
  //   ptr = &data[18];
  // }
  // else {
  //   ptr = data;
  // }

  ptr = data;

  if ((debugLevel == 3) || (debugLevel == 2)) {
    filterStr(str, ptr, 200);
    Log(DEBUG, "Sending: [%s]", str);
  }
  
  for (i = 0; i < socksCnt; i++) {
    if (sendto(socks[i], ptr, strlen(ptr), 0, (struct sockaddr *) &toaddrs[i], sizeof(toaddrs[i])) < 0) {
      Log(ERR, "writeUDP: SendTo Error");
    }
  }

  i = strlen(ptr) - 1;
  if ((ptr[i] == '\n') || (ptr[i] == '\r')) ptr[i--] = 0;
  if ((ptr[i] == '\n') || (ptr[i] == '\r')) ptr[i] = 0;
  Log(DATA, ptr);
}

// ----- TTY Port Management -----

int openTTY()
{
  if (debugLevel == 1) {
    // Nothing to do...
    Log(DEBUG, "openTTY: Faking serial port....\n");
  }
  else {
    if (debugLevel == 2) {
      serialPort = open(tty, O_RDWR | O_NOCTTY | O_NDELAY);
    }
    else {
      serialPort = open(tty, O_RDONLY | O_NOCTTY | O_NDELAY);
    }

    if (serialPort == -1) {
      Log(ERR, "openTTY: Unable to open serial port");
      return 1;
    }

    fcntl(serialPort, F_SETFL, 0);

    tcgetattr(serialPort, &serialConfig);

    switch (baud) {
    case 115200:
      cfsetispeed(&serialConfig, B115200);
      cfsetospeed(&serialConfig, B115200);
      break;
    case 38400:
      cfsetispeed(&serialConfig, B38400);
      cfsetospeed(&serialConfig, B38400);
      break;
    case 19200:
      cfsetispeed(&serialConfig, B19200);
      cfsetospeed(&serialConfig, B19200);
      break;
    case 9600:
      cfsetispeed(&serialConfig, B9600);
      cfsetospeed(&serialConfig, B9600);
      break;
    case 4800:
      cfsetispeed(&serialConfig, B4800);
      cfsetospeed(&serialConfig, B4800);
      break;
    case 2400:
      cfsetispeed(&serialConfig, B2400);
      cfsetospeed(&serialConfig, B2400);
      break;
    case 1200:
      cfsetispeed(&serialConfig, B1200);
      cfsetospeed(&serialConfig, B1200);
      break;
    case 600:
      cfsetispeed(&serialConfig, B600);
      cfsetospeed(&serialConfig, B600);
      break;
    case 300:
      cfsetispeed(&serialConfig, B300);
      cfsetospeed(&serialConfig, B300);
      break;
    default:
      Log(ERR, "openTTY: Invalid baud rate %d", baud);
      return 1;
    }

    serialConfig.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Raw input
    serialConfig.c_oflag &= ~OPOST;                          // Raw output

    serialConfig.c_iflag &= ~(IXON | IXOFF | IXANY);  // No XON XOFF Flow control

    serialConfig.c_cflag &= ~CSIZE;   // Clear bit size, set below at 8 bits (CS8)
    serialConfig.c_cflag &= ~PARENB;  // No parity
    serialConfig.c_cflag &= ~CSTOPB;  // One stop bit
    serialConfig.c_cflag &= ~CRTSCTS;  // No hardware flow control
    serialConfig.c_cflag |= (CLOCAL | CREAD | CS8); // Local port, enable Read, 8 bits

    if (tcsetattr(serialPort, TCSAFLUSH, &serialConfig)) {
      Log(ERR, "openTTY: Unable to set serial port parameters");
      return 1;
    }

  }

  buffIdx = 0;
  buffCnt = 0;

  if (debugLevel == 2) {
    Log(DEBUG, "openTTY: Serial port initialized: %d.", serialPort);
  }

  return 0;
}

void writeTTY(char *data)
{
  if (debugLevel) Log(DEBUG, "writeTTY: Sending [%s] to serial port", data);
  if (write(serialPort, data, strlen(data)) != strlen(data)) {
    Log(ERR, "writeTTY: Unable to write data to serial port");
  }
}

void readTTY()
{
  char *dataPtr;
  int dataCnt;

  if (debugLevel == 1) {
    fgets(data, sizeof(data) - 1, stdin);
  }
  else {
    dataPtr = data;
    dataCnt = 0;
    while (1) {
      while ((buffCnt > 0) && (buffIdx < buffCnt)) {
        char ch = buff[buffIdx++];
        *dataPtr++ = ch;
	      if (debugLevel) Log(DEBUG, "readTTY: Got <%d>", ch);
        if (++dataCnt >= (sizeof(data) - 1)) {
          *dataPtr = 0;
          return;
        }
        if (ch == '\n') {
          *dataPtr = 0;
          return;
        }
      }

      // if (debugLevel) Log(DEBUG, "readTTY: Reading from serial port...");
      buffCnt = read(serialPort, buff, sizeof(buff) - 1);
      if (buffCnt < 0) Log(ERR, "readTTY: Unable to read data from serial port");
      if (debugLevel && buffCnt) Log(DEBUG, "readTTY: Got %d chars", buffCnt);

      buffIdx = 0;
    }
  }
}

// ----- Main Program -----

int main(int argc, char **argv)
{

  if ((argc < 2) || (argc > 3)) {
    fprintf(stderr, "Usage: pdox <config filename> [debug level]\n");
    return 1;
  }

  debugLevel = 0;

  if (argc == 3) {
    debugLevel = atoi(argv[2]);

    if (debugLevel == 0) {
      fprintf(stderr, 
        "Available debug levels: \n\n"
        "1 - Fake serial port\n"
        "2 - Fake serial receive\n"
        "3 - Echo data sent to UDP port\n");
      return 1;
    }
  }

  openLog();

  if (debugLevel == 0) skeletonDaemon();

  Log(INFO, "Paradox Data Gathering Bootstrap");

  if (readConfig(argv[1])) { closeLog(); return 1; }
  if (openUDP()) { closeLog(); return 1; }
  if (openTTY()) { closeLog(); return 1; }
  if (openMQTT()) { closeLog(); return 1; }
  //if (initTimer()) { closeLog(); return 1; }

  strcpy(data, "Paradox Data Gathering Bootstrap\n");
  writeUDP();

  while (1) {
    if (debugLevel == 2) {
      char data[100];
      putchar('>');
      fgets(data, sizeof(data) - 1, stdin);
      writeTTY(data);
    }

    readTTY();
    if (!((strlen(data) == 1) && (data[0] == '\n'))) {
      writeUDP();
      sendMQTT();
    }
  }
}
