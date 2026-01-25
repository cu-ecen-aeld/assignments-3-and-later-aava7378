#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
	openlog("writer", LOG_PID,LOG_USER);
	if(argc !=3)
	{
		syslog(LOG_ERR, "incorrect args. Should be something like: %s <writefile> <writestr>",argv[0]);
		closelog();
		return 1;
	}

	const char* writef = argv[1];
	const char* writestr = argv[2];

	FILE *f = fopen(writef, "w");
	if(!f)
	{
		syslog(LOG_ERR, "error opening file '%s' : '%s'", writef, strerror(errno));
		closelog();
		return 1;
	}

	syslog(LOG_DEBUG, "Writing %s to %s", writestr, writef);

	if (fprintf(f, "%s", writestr) < 0)
	{
		syslog(LOG_ERR, "error writing to file '%s': %s", writef, strerror(errno));
		fclose(f);
		closelog();
		return 1;
	}

	if (fclose(f) != 0)
	{
		syslog(LOG_ERR, "error closing file '%s': %s", writef, strerror(errno));
		closelog();
		return 1;
	}

	closelog();
	return 0;
}
