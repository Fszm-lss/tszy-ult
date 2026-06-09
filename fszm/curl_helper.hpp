#pragma once

#include <map>
#include <string>
#include <curl/curl.h>
// use -lcurl while link
// install libcurl4-openssl-dev

namespace fszm {

typedef void (*CURL_SETOPT_EXTEND)(CURL*);

class CurlHelper
{
public:
	typedef std::map<std::string, std::string> StringMap;

	static std::string doGet(const char* url, int timeout = 1000, int* error = 0, 
		int cookie_mode = 0, bool verify_ssl = false, CURL_SETOPT_EXTEND SetOptExtend = nullptr)
	{
	    std::string result;
	    if (error) *error = -1;
	    if (!url || url[0] == '\0') return result;
	    
	    CURL* curl = curl_easy_init();
	    if (curl)
	    {
	        SetOptBasic(curl, url, timeout, verify_ssl);
		    SetOptCookie(curl, cookie_mode);
		    if (SetOptExtend) (*SetOptExtend)(curl);
	        
	        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlHelper::WriteMemoryCallback);
	        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
	        CURLcode res = curl_easy_perform(curl);
	       	if (error) *error = res;
	        curl_easy_cleanup(curl);
	    }
	    return result;
	}

	static std::string doPost(const char* url, const char* postJson, int timeout = 1000, int* error = 0, 
		int cookie_mode = 0, bool verify_ssl = false, CURL_SETOPT_EXTEND SetOptExtend = nullptr)
	{
	    std::string result;
	    if (error) *error = -1;
	    if (!url || url[0] == '\0') return result;
		if (!postJson || postJson[0] == '\0') return result;
	    
	    CURL* curl = curl_easy_init();
	    if (curl)
	    {
	        SetOptBasic(curl, url, timeout, verify_ssl);
		    SetOptCookie(curl, cookie_mode);
		    if (SetOptExtend) (*SetOptExtend)(curl);
	        
			struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postJson);

	        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlHelper::WriteMemoryCallback);
	        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
	        CURLcode res = curl_easy_perform(curl);
	       	if (error) *error = res;
			curl_slist_free_all(headers);
	        curl_easy_cleanup(curl);
	    }
	    return result;
	}

	static std::string doPost(const char* url, const StringMap& postMap, int timeout = 1000, int* error = 0,
		int cookie_mode = 0, bool verify_ssl = false, CURL_SETOPT_EXTEND SetOptExtend = nullptr)
	{
	    std::string result;
	    if (error) *error = -1;
	    if (!url || url[0] == '\0') return result;
		if (postMap.empty()) return result;
	    
	    CURL* curl = curl_easy_init();
	    if (curl)
	    {
	        SetOptBasic(curl, url, timeout, verify_ssl);
		    SetOptCookie(curl, cookie_mode);
		    if (SetOptExtend) (*SetOptExtend)(curl);

		    curl_easy_setopt(curl, CURLOPT_POST, 1L);
			if (!postMap.empty())
			{
				std::string param;
				StringMap::const_iterator it = postMap.begin();
				for ( ; it != postMap.end(); ++it) {
					char* escaped_key = curl_easy_escape(curl, it->first.c_str(), 0);
      				char* escaped_val = curl_easy_escape(curl, it->second.c_str(), 0);

					if (escaped_key && escaped_val) {
						if (it != postMap.begin()) param += "&";
						param += std::string(escaped_key) + "=" + std::string(escaped_val);
					}

					if (escaped_key) curl_free(escaped_key);
      				if (escaped_val) curl_free(escaped_val);
				}
		    	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, param.c_str());
			}
		    
	        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlHelper::WriteMemoryCallback);
	        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
	        CURLcode res = curl_easy_perform(curl);
	       	if (error) *error = res;
	        curl_easy_cleanup(curl);
	    }
	    return result;
	}

	static int downloadFile(const char* url, const char* path, int timeout = 1000, bool verify_ssl = false, 
		CURL_SETOPT_EXTEND SetOptExtend = nullptr)
	{
	    int rc = -1;
	    if (!url || url[0] == '\0' || !path || path[0] == '\0') return -1;
	    
	    CURL* curl = curl_easy_init();
	    if (curl)
	    {
	    	FILE* file = fopen(path,"wb");
	    	if (file)
	    	{
	    		SetOptBasic(curl, url, timeout, verify_ssl);
			    SetOptCookie(curl, 2);
			    if (SetOptExtend) (*SetOptExtend)(curl);

			    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlHelper::WriteFileCallback);
			    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
		        rc = curl_easy_perform(curl);
		       	fclose(file);
	    	}
	        curl_easy_cleanup(curl);
	    }
	    return rc;
	}

private:
	static void SetOptBasic(CURL* curl, const char* url, int timeout_ms, bool verify_ssl = false)
	{
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ssl ? 1L : 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl ? 2L : 0L);
	}

	// cookie_mode: 1, save while login; 2, load when already login
	static void SetOptCookie(CURL* curl, int cookie_mode)
	{
		if (1 == cookie_mode)      // save
			curl_easy_setopt(curl, CURLOPT_COOKIEJAR, "cookiestore");
	    else if (2 == cookie_mode) // load
			curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "cookiestore");
	}

	static size_t WriteMemoryCallback(void *data, size_t size, size_t nmemb, void *obj)
	{ 
		size_t realsize = size * nmemb; 
		std::string* str = (std::string*) obj;
	    str->append((char*) data, realsize);
	    return realsize;
	}

	static size_t WriteFileCallback(void *data, size_t size, size_t nmemb, void *obj)
	{ 
		size_t written = fwrite(data, size, nmemb, (FILE*) obj);
		return written;
	}
};

}
