/*
 * Copyright (c) 2022, Kalopa Robotics Limited.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of Kalopa Robotics nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY KALOPA ROBOTICS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL KALOPA ROBOTICS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ABSTRACT
 * Set the system time by reading from a serially-attached GPS device.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

#define BUFFER_SIZE		512

#define ST_WAITNL		0
#define ST_WAITDL		1
#define ST_CAPTURE		2

/*
 * Translate table to convert a baud rate into a B-number for the kernel.
 */
struct	speed	{
	int	value;
	int	code;
} speeds[] = {
	{50, B50},
	{75, B75},
	{110, B110},
	{134, B134},
	{150, B150},
	{200, B200},
	{300, B300},
	{600, B600},
	{1200, B1200},
	{1800, B1800},
	{2400, B2400},
	{4800, B4800},
	{9600, B9600},
	{19200, B19200},
	{38400, B38400},
	{57600, B57600},
	{115200, B115200},
	{230400, B230400},
	{0, 0}
};

int	verbose;
char	rdata[BUFFER_SIZE];
char	input[BUFFER_SIZE];

void	process(int);
void	gps_line();
int	crack(char *, char *[], int);
int	getvalue(char *, int);
void	usage();

/*
 * All life starts here...
 */
int
main(int argc, char *argv[])
{
	int i, fd, baud = 9600;
	char *cp, *device = "/dev/ttyu0";
	struct termios tios;

	/*
	 * Do the command-line arguments.
	 */
	verbose = 0;
	while ((i = getopt(argc, argv, "s:l:v")) != EOF) {
		switch (i) {
		case 's':
			baud = atoi(optarg);
			break;

		case 'l':
			device = optarg;
			break;

		case 'v':
			verbose = 1;
			break;

		default:
			usage();
			break;
		}
	}
	if (verbose)
		printf("GPS device: %s, speed: %d.\n", device, baud);
	if ((fd = open(device, O_RDONLY|O_NOCTTY)) < 0) {
		fprintf(stderr, "gps_time: ");
		perror(device);
		exit(1);
	}
	/*
	 * Get and set the tty parameters.
	 */
	if (verbose)
		printf("Setting serial I/O parameters.\n");
	if (tcgetattr(fd, &tios) < 0) {
		perror("gps_time: tcgetattr");
		exit(1);
	}
	for (i = 0; speeds[i].value > 0; i++)
		if (speeds[i].value == baud)
			break;
	if (speeds[i].value == 0) {
		fprintf(stderr, "gps_time: invalid baud rate: %d\n", baud);
		exit(1);
	}
	tios.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
	tios.c_oflag = 0;
	tios.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
	tios.c_cflag &= ~(CSIZE|PARENB);
	tios.c_cflag |= CS8;
	tios.c_cc[VMIN] = 1;
	tios.c_cc[VTIME] = 0;
	cfsetispeed(&tios, speeds[i].code);
	cfsetospeed(&tios, speeds[i].code);
	if (tcsetattr(fd, TCSANOW, &tios) < 0) {
		perror("gps_time: tcsetattr");
		exit(1);
	}
	/*
	 * Convert the fd into a FILE pointer - let someone else
	 * do the buffering...
	 */
	if (verbose)
		printf("Opening file descriptor as a file.\n");
	/*
	 * Read each character from the serial device, and process it.
	 */
	while ((i = read(fd, cp = rdata, BUFFER_SIZE)) > 0) {
		while (i-- > 0)
			process(*cp++);
	}
	if (verbose)
		printf("Program terminated normally.\n");
	exit(0);
}

/*
 * Process a single character of serial data.
 */
void
process(int ch)
{
	static int inpos = 0;
	static int state = ST_WAITNL;

	if (ch == '\n' || ch == '\r') {
		/*
		 * Saw a CR/NL. Process a line, if we have one.
		 */
		if (state == ST_CAPTURE) {
			input[inpos++] = '\0';
			gps_line();
		}
		state = ST_WAITDL;
		return;
	}
	/*
	 * If we're waiting for a dollar-sign, then if we get one
	 * then we're good. Otherwise, any other character will
	 * force us into waiting for a new CR/LF.
	 */
	if (state == ST_WAITDL) {
		inpos = 0;
		if (ch == '$')
			state = ST_CAPTURE;
		else
			state = ST_WAITNL;
		return;
	}
	/*
	 * Capture the character, if we're in the right state.
	 */
	if (state != ST_CAPTURE)
		return;
	if (inpos >= sizeof(input) - 2) {
		/*
		 * Line is too long. Dump it.
		 */
		state = ST_WAITNL;
		return;
	}
	input[inpos++] = ch;
}

/*
 * Handle a single line of GPS data. Really we only care about GPRMC lines.
 */
void
gps_line()
{
	int csum = 0;
	char *cp, *args[20];
	struct timeval tval;
	struct tm tm;

	if (verbose)
		printf("GPS: [%s]\n", input);
	if (strncmp(input, "GPRMC", 5) != 0) {
		if (verbose)
			printf("Waiting for an RMC message - ignoring this one...\n");
		return;
	}
	/*
	 * Skip to the end of the sentence, just before the checksum.
	 */
	for (cp = input; *cp; cp++) {
		if (*cp == '*')
			break;
		csum ^= *cp;
	}
	/*
	 * No checksum? Dunno what that was - ditch it.
	 */
	if (*cp != '*') {
		if (verbose)
			printf("?Badly formed NMEA sentence - ignoring...\n");
		return;
	}
	/*
	 * Compare the checksum we computed versus the one at the
	 * end of the sentence.
	 */
	if (strtol(cp + 1, NULL, 16) != csum) {
		if (verbose)
			printf("?Invalid checksum - ignoring...\n");
		return;
	}
	*cp = '\0';
	if (verbose)
		printf("Checksum is good.\n");
	/*
	 * Split the sentence into its arguments (comma-based).
	 */
	if (crack(input, args, 20) != 13) {
		if (verbose)
			printf("Incorrect number of RMC paramaters in sentence...\n");
		return;
	}
	if (verbose) {
		/*
		 * Show the encoded time and date fields.
		 */
		printf("GPS Time: %s\n", args[1]);
		printf("GPS Date: %s\n", args[9]);
	}
	/*
	 * Fill a TM struct based on the data in the sentence. Note
	 * that the year is a bit Y2K, but what can ya do.
	 */
	tm.tm_sec = getvalue(args[1] + 4, 2);
	tm.tm_min = getvalue(args[1] + 2, 2);
	tm.tm_hour = getvalue(args[1], 2);
	tm.tm_mday = getvalue(args[9], 2);
	tm.tm_mon = getvalue(args[9] + 2, 2) - 1;
	tm.tm_year = getvalue(args[9] + 4, 2) + 100;
	tm.tm_isdst = 0;
	tm.tm_gmtoff = 0L;
	tval.tv_sec = mktime(&tm);
	tval.tv_usec = getvalue(args[1] + 7, 3) * 1000;
	if (verbose)
		printf("Setting time to %s", ctime(&tval.tv_sec));
	/*
	 * Set the system time.
	 */
	if (settimeofday(&tval, NULL) == 0) {
		if (verbose)
			printf("Time set successfully. Operation complete.\n");
		exit(0);
	}
	perror("gps_time: settimeofday");
}

/*
 * Crack a comman-separated string into components.
 */
int
crack(char *strp, char *argv[], int maxargs)
{
	int n;
	char *cp;

	if (strp == NULL || *strp == '\0')
		return(0);
	for (n = 0; n < maxargs; n++) {
		while (isspace(*strp))
				strp++;
		if (*strp == '\0')
			return(n);
		argv[n] = strp;
		if ((cp = strchr(strp, ',')) != NULL)
				*cp++ = '\0';
		strp = cp;
		if (strp == NULL || *strp == '\0')
				break;
	}
	return(n + 1);
}

/*
 * Get a numeric value from a string.
 */
int
getvalue(char *strp, int ndigits)
{
	int value = 0;

	while (ndigits-- && isdigit(*strp))
		value = value * 10 + *strp++ - '0';
	return(value);
}

/*
 * Usage message & exit.
 */
void
usage()
{
	fprintf(stderr, "Usage: gps_time [-s 9600][-l /dev/ttyu0][-v]\n");
	exit(2);
}
