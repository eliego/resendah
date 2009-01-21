#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <regex.h>
#include <signal.h>

void error(char *mesg, int die);
int getconnectedsocket(char *hostname, int port);
int recvcode(int sock);
int sendcmd(int sock, char *cmd);
int filter(const struct dirent* fil);
int compar(const void* a, const void* b) { return 0; }
int smtp_connect(int* sock, char* host);
int smtp_quit(int* sock);

int main(int argc, char** argv) {
  FILE* fil;
  struct dirent** filer;
  int n, i, sock, r, q, failed = 0;
  char data[502], temp[501], from[61], filnamn[502];

  // Ignore SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  // Usage
  if (argc < 4) {
    fprintf(stdout, "Usage: ./resendah <dir> <SMTP> <receiverv>\n");
    exit(0); }

  // List files
  if ((n = scandir(argv[1], &filer, filter, compar)) < 1)
    error("No message files in dir", 1);

  // Connect to SMTP
  if(smtp_connect(&sock, argv[2]) == -1)
    error("Couldn't connect to SMTP", 1);

  // Loop through files
  for (i = 0; i < n; i++) {
    strncpy(filnamn, argv[1], 500 - strlen(filer[i]->d_name));
    if (filnamn[strlen(filnamn) - 1] != '/')
      strcat(filnamn, "/");
    strcat(filnamn, filer[i]->d_name);
    fil = fopen(filnamn, "r");

    if (argc > 4 && strcmp(argv[4], "--debug") == 0)
      fprintf(stderr, "Debug: Processing %d out of %d (file %s)\n", i + 1, n, filer[i]->d_name);

    // Get from-address
    *data = '\0';
    while (sscanf(data, "Return-Path: <%60[^<> ]>", from) != 1)
      if (fgets(data, 500, fil) == NULL) {
	    *from = '\0';
      	break;
      }
    
    if (strlen(from) == 0) {
      snprintf(data, 500, "Couldn't get Return-Path-header from %s, skipping...", filer[i]->d_name);
      error(data, 0);
      fclose(fil);
      continue;
    }

    // Reset file
    rewind(fil);

    // Send MAIL
    snprintf(data, 500, "MAIL FROM: <%s>", from);
    sendcmd(sock, data);
    if ((r = recvcode(sock)) != 250) {
      snprintf(data, 500, "SMTP didn't accept From-header from %s (%s resulted in %d), skipping...", filer[i]->d_name, from, r);
      error(data, 0);
      fclose(fil);
      sendcmd(sock, "RSET");
      if (recvcode(sock) != 250) {
	error("SMTP didn't accept RSET, reconnecting...", 0);
	if (smtp_quit(&sock) == -1)
	  error("SMTP didn't accept QUIT", 0);
	if (smtp_connect(&sock, argv[2]) == -1)
	  error("Couldn't connect to SMTP", 1);
      }
      continue;
    }

    // Send RCPT
    snprintf(data, 500, "RCPT TO: <%s>", argv[3]);
    sendcmd(sock, data);
    if ((r = recvcode(sock)) != 250 && r != 251)
      error("SMTP didn't accept receipient", 1);
    
    // Send DATA
    sendcmd(sock, "DATA");
    if (recvcode(sock) != 354)
      error("SMTP didn't accept DATA", 1);

    // Send mail from file
    while (fgets(data, 500, fil) != NULL) {
      if (*data == '.') {
	    strcpy(temp, data);
	    *(data + 1) = '\0';
	    strcat(data, temp);
      }
      r = q = 0;
      while (r < strlen(data)) {
	if ((q = send(sock, data + r, strlen(data + r), 0)) == -1) {
	  error("SMTP kicked us out, reconnecting...", 0);
	  if (smtp_connect(&sock, argv[2]) == -1)
	    error("Couldn't connect to SMTP", 1);
	  failed = 1;
	  break;
	}
	r += q;
      }
      if (failed == 1)
	break;
    }

    if (failed == 1) {
      failed = 0;
      i--;
      continue;
    }

    sendcmd(sock, "\r\n.");
    if ((r = recvcode(sock)) != 250) {
      snprintf(data, 500, "SMTP didn't accept the mailfile (said %d)", r);
      error(data, 1);
    }	

    // Send RESET
    sendcmd(sock, "RSET");
    if ((r = recvcode(sock)) != 250) {
      snprintf(data, 500, "SMTP didn't accept RSET (said %d), reconnecting...", r);
      error(data, 0);
      if (smtp_quit(&sock) == -1)
	error("SMTP didn't accept QUIT", 0);
      if (smtp_connect(&sock, argv[2]) == -1)
	error("Couldn't connect to SMTP", 1);
    }
    
    // Closing file
    fclose(fil);
  }
  
  // Send QUIT
  if (smtp_quit(&sock) == -1)
    error("SMTP didn't accept QUIT", 0);

  fprintf(stdout, "Finished! Processed %d files.\n", n);
  return 0;
}


int filter(const struct dirent* fil) {
  char* c;
  for (c = fil->d_name; *c != '\0'; c++)
    if (!isdigit(*c))
      return 0;

  return 1;
}

void error(char *mesg, int die) {
  fprintf(stderr, "Resendah error: %s%s\n", mesg, (die == 0) ? ("") : (", exiting"));	
  if (die != 0)
    exit(die);
  else
    fflush(stderr);
  
  return;
}

int getconnectedsocket(char *hostname, int port) {
	struct hostent *hostdata;
	struct sockaddr_in host;
	int sock;

	if ((hostdata = gethostbyname(hostname)) == NULL) {
		return -1;
	}

	host.sin_family = AF_INET;
	host.sin_port = htons(port);
	host.sin_addr = *((struct in_addr *)hostdata->h_addr);
	memset(&(host.sin_zero), '\0', 8);
	
	if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
		return -1;

	}
	
	if (connect(sock, (struct sockaddr *)&host, sizeof(struct sockaddr)) == -1) {
		return -1;
	}

	// Woho! We did it! C6 - here i come!!!
	return sock;
}

int recvcode(int sock) {
  char code[3];
  int res, i = 0;
  
  if ((res = recv(sock, code, 3, 0)) <= 0)
		return res;
  
  code[3] = '\0';
 
  do {
    while (i != '\r')
      if ((res = recv(sock, &i, 1, 0)) <= 0)
	return res;

    if ((res = recv(sock, &i, 1, 0)) <= 0)
      return res;
  } while (i != '\n');

  // Convert to int
  res = 0;
  for (i = 0; i < strlen(code); i++) {
    if (isdigit(code[i]) == 0)
      return -1;
    res *=10;
    res += code[i] - '0';
  }
  
  return res;
}

int sendcmd(int sock, char *cmd) {
  char command[504];
  int sent = 0;
  int temp = 0;
  
  strcpy(command, cmd);
  strcat(command, "\r\n");
  
  while (sent < strlen(command)) {
    if ((temp = send(sock, command + sent, strlen(command + sent), 0)) == -1)   
      return -1;
    
    sent += temp;
  }	
  return 0;
}

int smtp_connect(int* sock, char* host) {
  char hostname[61];
  char data[501];

  // Get hostname
  gethostname(hostname, 60);
  
  // Connect to smtp
  if ((*sock = getconnectedsocket(host, 25)) == -1)
    return -1;

  // Receive banner
  if (recvcode(*sock) != 220)
    return -1;

  // Send HELO
  sprintf(data, "HELO %s", hostname);
  sendcmd(*sock, data);
  if (recvcode(*sock) != 250)
    return -1;

  return 0;
}

int smtp_quit(int* sock) {
  sendcmd(*sock, "QUIT");
  if (recvcode(*sock) != 221)
    return -1;

  return 0;
}
