#include <curl/curl.h>
#include <curl/easy.h>
#include <libguile.h>

#include "smtp.h"
#include "log.h"

struct tracked_string {
	char *pt;
	char *src;
	};

static size_t reader(void *ptr, size_t size, size_t nmemb,
		void *userp) {
	struct tracked_string *src = (struct tracked_string *)userp;
	size_t limit = size * nmemb;
	size_t len;
	len = strlen(src->pt);
	if (len > limit) len = limit;
	if (len == 0) return 0;
	memcpy(ptr, src->pt, len);
	src->pt += len;
	return len;
	}

static SCM smtp_send(SCM url, SCM from, SCM recipients,
		SCM username, SCM password, SCM payload) {
	CURL *curl;
	CURLcode res;
	SCM out;
	struct tracked_string s_payload;
	char *s_username, *s_password, *s_url, *s_from, *buf;
	struct curl_slist *s_recipients = NULL;
	curl = curl_easy_init();
	if (curl == NULL) {
		log_msg("smtp_send: curl init failed\n");
		return SCM_BOOL_F;
		}
	s_username = scm_to_utf8_string(username);
	curl_easy_setopt(curl, CURLOPT_USERNAME, s_username);
	s_password = scm_to_utf8_string(password);
	curl_easy_setopt(curl, CURLOPT_PASSWORD, s_password);
	s_url = scm_to_utf8_string(url);
	curl_easy_setopt(curl, CURLOPT_URL, s_url);
	curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1);
	s_from = scm_to_utf8_string(from);
	curl_easy_setopt(curl, CURLOPT_MAIL_FROM, s_from);
	while (recipients != SCM_EOL) {
		buf = scm_to_utf8_string(SCM_CAR(recipients));
		s_recipients = curl_slist_append(s_recipients, buf);
		free(buf);
		recipients = SCM_CDR(recipients);
		}
	curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, s_recipients);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, reader);
	s_payload.src = scm_to_utf8_string(payload);
	s_payload.pt = s_payload.src;
	curl_easy_setopt(curl, CURLOPT_READDATA, (void *)&s_payload);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		log_msg("smtp_send: %s\n", curl_easy_strerror(res));
		out = SCM_BOOL_F;
		}
	else out = SCM_BOOL_T;
	curl_slist_free_all(s_recipients);
	curl_easy_cleanup(curl);
	free(s_payload.src);
	free(s_username);
	free(s_password);
	free(s_url);
	free(s_from);
	return out;
	}

void init_smtp(void) {
	scm_c_define_gsubr("smtp-send", 6, 0, 0, smtp_send);
	}

void shutdown_smtp() {
	}

