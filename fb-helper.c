/*
 * Description: This is intended as a helper script for fb only.
 *
 * Synopsis: ./fb-helper <action> <URL> <file>
 *
 * action can be:
 * d = download <URL>
 * u = upload <file> to <URL>
 *
 * Author: Florian "Bluewind" Pritz <flo@xssn.at>
 *
 * Licensed under GPLv3
 *   (see COPYING for full license text)
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <libgen.h>
#include <string.h>
#include <stdlib.h>

#include <curl/curl.h>
#include <curl/easy.h>

#define FORMAT_ARRAY_SIZE 5
#define FORMAT_BYTES_BUFFER 64

/* struct which holds the persistent data for progress_callback */
struct progressData {
	struct timeval starttime;
	struct timeval last;
	double ullast;
	int lastStringLength;
};

/* load the contents of file fn into data */
int load_file(const char *fn, char **data, size_t *data_size)
{
	FILE *fp;
	size_t buf_size = 1024*1024; /* use 1MiB chunks */

	fp = fopen(fn, "rb");
	if (fp == NULL) {
		perror("load_file");
		return 1;
	}

	/* read the file in buf_size chunks and appened the data to *data */
	while (!feof(fp)) {
		*data = realloc(*data, *data_size + buf_size);
		if (*data == NULL) {
			perror("load_file");
			return 1;
		}

		*data_size += fread(*data + *data_size, sizeof(char), buf_size, fp);

		if (ferror(fp)) {
			perror("load_file");
			return 1;
		}
	}

	fclose(fp);

	return 0;
}

void format_bytes(double bytes, char *buf)
{
	double size = bytes;
	int suffix_pos = 0;
	static const char *suffix[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
	static const int suffix_count = sizeof(suffix) / sizeof(suffix[0]);
	static const double boundary = 2048.0;

	for(suffix_pos = 0; suffix_pos + 1 < suffix_count; suffix_pos++) {
		if(size <= boundary && size >= -boundary) {
			break;
		}
		size /= 1024.0;
	}

	// don't print decimals for bytes
	if (suffix_pos != 0)
		snprintf(buf, FORMAT_BYTES_BUFFER, "%.2f%s", size, suffix[suffix_pos]);
	else
		snprintf(buf, FORMAT_BYTES_BUFFER, "%.0f%s", size, suffix[suffix_pos]);
}


int progress_callback(
	void *cb_data,
	double dltotal, double dlnow,
	double ultotal, double ulnow
){
	struct timeval now;
	struct progressData *data = (struct progressData *)cb_data;
	double timeSpent = 0;
	int printed = 0;
	char speed[FORMAT_BYTES_BUFFER];
	char total[FORMAT_BYTES_BUFFER];

	if (0 == ulnow)
		return 0;

	/* upload complete; clean up */
	if (ulnow >= ultotal) {
		fprintf(stderr, "%*s\r", data->lastStringLength + 1, "");
		return 0;
	}

	gettimeofday(&now, NULL);

	/* calculate time between this and the last call in seconds */
	timeSpent =
		(double)(now.tv_sec - data->last.tv_sec) +
		(double)(now.tv_usec - data->last.tv_usec) / 1E6;

	format_bytes((ulnow - data->ullast) / timeSpent, (char *)&speed);
	/* don't refresh too often, catch if time went backwards */
	if (timeSpent < 0.2 && timeSpent > -1.0)
		return 0;

	format_bytes(ulnow, (char *)&total);

	/* print the progress */
	printed = fprintf(stderr,
		"\r%s/s uploaded: %.1f%% = %s",
		speed, /* upload speed */
		ulnow * 100.0 / ultotal, /* percent uploaded */
		total /* total data uploaded */
		);

	/* pad the string if the last one was longer to remove left over characters */
	if (data->lastStringLength > printed)
		fprintf(stderr, "%*s", data->lastStringLength - printed, "");

	/* save current values for the next run */
	data->ullast = ulnow;
	data->last = now;
	data->lastStringLength = printed;

	return 0;
}

int main(int argc, char *argv[])
{
	CURL *curl;
	CURLcode res;

	struct curl_httppost *formpost = NULL;
	struct curl_httppost *lastptr = NULL;
	struct curl_slist *headerlist = NULL;
	static const char buf[] = "Expect:";
	struct curl_forms forms[4];

	char *userAgent = "fb-client/"VERSION;

	struct progressData cb_data = {
		.starttime = {.tv_sec = 0, .tv_usec = 0},
		.last = {.tv_sec = 0, .tv_usec = 0},
		.ullast = 0.0,
		.lastStringLength = 0
	};

	char *mode = NULL;
	char *data = NULL;
	char *url = NULL;
	char *file = NULL;

	int ret = 0;

	/* process arguments */
	if(argc == 1)
		return 1;

	mode = argv[1];

	if (strncmp(mode, "u", 1) == 0)
		file = argv[3];

	url = argv[2];

	/* initialize curl */
	if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
		fprintf(stderr, "Error initializing curl");
		return 10;
	}

	curl = curl_easy_init();
	if(!curl) {
		fprintf(stderr, "Error initializing curl");
		return 1;
	}

	/* if we have a file to upload, add it as a POST request */
	if (file) {
		struct stat statbuf;

		if(stat(file, &statbuf) == -1) {
			fprintf(stderr, "fb-helper: %s: ", file);
			perror(NULL);
			return 1;
		}

		/* load files with 0 size (/proc files for example) into memory so we can
		 * determine their real length */
		if (statbuf.st_size == 0) {
			size_t data_size = 0;

			if (load_file(file, &data, &data_size) != 0) {
				return 1;
			}

			forms[0].option = CURLFORM_BUFFER;
			forms[0].value  = basename(file);
			forms[1].option = CURLFORM_BUFFERPTR;
			forms[1].value  = data;
			forms[2].option = CURLFORM_BUFFERLENGTH;
			forms[2].value  = (char *)data_size;
			forms[3].option = CURLFORM_END;
		} else {
			forms[0].option = CURLFORM_FILE;
			forms[0].value  = file;
			forms[1].option = CURLFORM_END;
		}

		/* Fill in the file upload field */
		curl_formadd(&formpost,
			 &lastptr,
			 CURLFORM_COPYNAME, "file",
			 CURLFORM_ARRAY, forms,
			 CURLFORM_END);
		curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);

		/* display progress bar */
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
		curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &cb_data);
		curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
	}

	/* initialize custom header list (stating that Expect: 100-continue is not
		 wanted */
	headerlist = curl_slist_append(headerlist, buf);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);

	/* use .netrc settings for authentication if available */
	curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);


	/* bail if the upload stalls for 30 seconds */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)1);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)30);

	/* save time for progress calculation */
	gettimeofday(&cb_data.starttime, NULL);

	/* run the request */
	res = curl_easy_perform(curl);

	/* handle curl errors */
	if (res != 0) {
		fprintf(stderr, "\n%s\n", curl_easy_strerror(res));
		ret = 1;
	}

	/* cleanup */
	curl_easy_cleanup(curl);

	if (formpost)
		curl_formfree(formpost);

	curl_slist_free_all (headerlist);
	curl_global_cleanup();
	free(data);

	return ret;
}
