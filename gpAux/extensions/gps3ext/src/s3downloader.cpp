#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <sstream>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "gps3ext.h"
#include "s3downloader.h"
#include "s3log.h"
#include "s3utils.h"

using std::stringstream;

OffsetMgr::OffsetMgr(uint64_t m, uint64_t c)
    : maxsize(m), chunksize(c), curpos(0) {
    pthread_mutex_init(&this->offset_lock, NULL);
}

Range OffsetMgr::NextOffset() {
    Range ret;
    pthread_mutex_lock(&this->offset_lock);
    if (this->curpos < this->maxsize) {
        ret.offset = this->curpos;
    } else {
        ret.offset = this->maxsize;
    }

    if (this->curpos + this->chunksize > this->maxsize) {
        ret.len = this->maxsize - this->curpos;
        this->curpos = this->maxsize;
    } else {
        ret.len = this->chunksize;
        this->curpos += this->chunksize;
    }
    pthread_mutex_unlock(&this->offset_lock);
    // std::cout<<ret.offset<<std::endl;
    return ret;
}

void OffsetMgr::Reset(uint64_t n) {
    pthread_mutex_lock(&this->offset_lock);
    this->curpos = n;
    pthread_mutex_unlock(&this->offset_lock);
}

BlockingBuffer::BlockingBuffer(string url, OffsetMgr *o)
    : sourceurl(url),
      status(BlockingBuffer::STATUS_EMPTY),
      eof(false),
      error(false),
      readpos(0),
      realsize(0),
      bufferdata(NULL),
      mgr(o) {
    this->nextpos = o->NextOffset();
    this->bufcap = o->Chunksize();
}

BlockingBuffer::~BlockingBuffer() {
    if (this->bufferdata) {
        free(this->bufferdata);
        pthread_mutex_destroy(&this->stat_mutex);
        pthread_cond_destroy(&this->stat_cond);
    }
};

bool BlockingBuffer::Init() {
    this->bufferdata = (char *)malloc(this->bufcap);
    if (!this->bufferdata) {
        S3ERROR("Failed to allocate Buffer, no enough memory?");
        return false;
    }

    pthread_mutex_init(&this->stat_mutex, NULL);
    pthread_cond_init(&this->stat_cond, NULL);
    return true;
}

// ret < len means EMPTY
uint64_t BlockingBuffer::Read(char *buf, uint64_t len) {
    // assert buf not null
    // assert len > 0, len < this->bufcap
    pthread_mutex_lock(&this->stat_mutex);
    while (this->status == BlockingBuffer::STATUS_EMPTY) {
        pthread_cond_wait(&this->stat_cond, &this->stat_mutex);
    }
    uint64_t left_data_length = this->realsize - this->readpos;
    int length_to_read = std::min(len, left_data_length);

    memcpy(buf, this->bufferdata + this->readpos, length_to_read);
    if (left_data_length >= len) {
        this->readpos += len;  // not empty
    } else {                   // empty, reset everything
        this->readpos = 0;

        if (this->status == BlockingBuffer::STATUS_READY) {
            this->status = BlockingBuffer::STATUS_EMPTY;
        }

        if (!this->EndOfFile()) {
            this->nextpos = this->mgr->NextOffset();
            pthread_cond_signal(&this->stat_cond);
        }
    }
    pthread_mutex_unlock(&this->stat_mutex);
    return length_to_read;
}

// returning -1 mearns error, pls don't set chunksize to uint64_t(-1) for now
uint64_t BlockingBuffer::Fill() {
    // assert offset > 0, offset < this->bufcap
    pthread_mutex_lock(&this->stat_mutex);
    while (this->status == BlockingBuffer::STATUS_READY) {
        pthread_cond_wait(&this->stat_cond, &this->stat_mutex);
    }
    uint64_t offset = this->nextpos.offset;
    uint64_t leftlen = this->nextpos.len;
    // assert this->status != BlockingBuffer::STATUS_READY
    uint64_t readlen = 0;
    this->realsize = 0;
    while (this->realsize < this->bufcap) {
        if (leftlen != 0) {
            readlen = this->fetchdata(offset, this->bufferdata + this->realsize,
                                      leftlen);
            // XXX fix the returning type
            if (readlen == -1) {
                S3DEBUG("Failed to fetch data from libcurl");
            } else {
                S3DEBUG("Got %llu bytes from libcurl", readlen);
            }
        } else {
            readlen = 0;  // EOF
        }
        if (readlen == 0) {  // EOF!!
            // if (this->realsize == 0) {
            this->eof = true;
            //}
            S3DEBUG("Reached the end of file");
            break;
        } else if (readlen == -1) {  // Error, network error or sth.
            // perror, retry
            this->error = true;
            // Ensure status is still empty
            // this->status = BlockingBuffer::STATUS_READY;
            // pthread_cond_signal(&this->stat_cond);
            S3ERROR("Failed to download file");
            break;
        } else {  // > 0
            offset += readlen;
            leftlen -= readlen;
            this->realsize += readlen;
            // this->status = BlockingBuffer::STATUS_READY;
        }
    }
    this->status = BlockingBuffer::STATUS_READY;
    pthread_cond_signal(&this->stat_cond);

    pthread_mutex_unlock(&this->stat_mutex);
    return (readlen == -1) ? -1 : this->realsize;
}

BlockingBuffer *BlockingBuffer::CreateBuffer(string url, string region,
                                             OffsetMgr *o,
                                             S3Credential *pcred) {
    BlockingBuffer *ret = NULL;
    if (url == "") return NULL;

    if (pcred) {
        ret = new S3Fetcher(url, region, o, *pcred);
    } else {
        ret = new HTTPFetcher(url, o);
    }

    return ret;
}

void *DownloadThreadfunc(void *data) {
    BlockingBuffer *buffer = reinterpret_cast<BlockingBuffer *>(data);
    uint64_t filled_size = 0;
    S3INFO("Downloading thread starts");
    do {
        if (QueryCancelPending) {
            S3INFO("Downloading thread is interrupted by GPDB");
            return NULL;
        }

        filled_size = buffer->Fill();
        // XXX fix the returning type
        if (filled_size == -1) {
            S3DEBUG("Failed to fill downloading buffer");
        } else {
            S3DEBUG("Size of filled data is %llu", filled_size);
        }
        if (buffer->EndOfFile()) break;
        if (filled_size == -1) {  // Error
            // retry?
            if (buffer->Error()) {
                break;
            } else {
                continue;
            }
        }
    } while (1);
    S3INFO("Downloading thread ended");
    return NULL;
}

Downloader::Downloader(uint8_t part_num)
    : num(part_num), o(NULL), chunkcount(0), readlen(0) {
    this->threads = (pthread_t *)malloc(num * sizeof(pthread_t));
    if (this->threads)
        memset((void *)this->threads, 0, num * sizeof(pthread_t));
    else {
        S3ERROR("Failed to malloc thread, no enough memory");
    }

    this->buffers = (BlockingBuffer **)malloc(num * sizeof(BlockingBuffer *));
    if (this->buffers)
        memset((void *)this->buffers, 0, num * sizeof(BlockingBuffer *));
    else {
        S3ERROR("Failed to malloc blocking buffer, no enough memory?");
    }
}

bool Downloader::init(string url, string region, uint64_t size,
                      uint64_t chunksize, S3Credential *pcred) {
    if (!this->threads || !this->buffers) {
        return false;
    }

    this->o = new OffsetMgr(size, chunksize);
    if (!this->o) {
        S3ERROR("Failed to create offset manager, no enough memory?");
        return false;
    }

    for (int i = 0; i < this->num; i++) {
        this->buffers[i] = BlockingBuffer::CreateBuffer(
            url, region, o, pcred);  // decide buffer according to url
        if (!this->buffers[i]->Init()) {
            S3ERROR("Failed to init blocking buffer");
            return false;
        }
        pthread_create(&this->threads[i], NULL, DownloadThreadfunc,
                       this->buffers[i]);
    }
    readlen = 0;
    chunkcount = 0;
    return true;
}

bool Downloader::get(char *data, uint64_t &len) {
    uint64_t filelen = this->o->Size();

RETRY:
    if (this->readlen == filelen) {
        len = 0;
        return true;
    }

    BlockingBuffer *buf = buffers[this->chunkcount % this->num];
    uint64_t tmplen = buf->Read(data, len);
    this->readlen += tmplen;
    if (tmplen < len) {
        this->chunkcount++;
        if (buf->Error()) {
            S3ERROR("Error occurs while downloading, skip");
            return false;
        }
    }

    // retry to confirm whether thread reading is finished or chunk size is
    // divisible by get()'s buffer size
    if (tmplen == 0) {
        goto RETRY;
    }
    len = tmplen;

    // S3DEBUG("Got %llu, %llu / %llu", len, this->readlen, filelen);
    return true;
}

void Downloader::destroy() {
    for (int i = 0; i < this->num; i++) {
        if (this->threads && this->threads[i]) pthread_cancel(this->threads[i]);
    }
    for (int i = 0; i < this->num; i++) {
        if (this->threads && this->threads[i])
            pthread_join(this->threads[i], NULL);
        if (this->buffers && this->buffers[i]) delete this->buffers[i];
    }
    if (this->o) delete this->o;
}

Downloader::~Downloader() {
    if (this->threads) free(this->threads);
    if (this->buffers) free(this->buffers);
}

// return the number of items
static uint64_t WriterCallback(void *contents, uint64_t size, uint64_t nmemb,
                               void *userp) {
    uint64_t realsize = size * nmemb;
    Bufinfo *p = reinterpret_cast<Bufinfo *>(userp);

    if (QueryCancelPending) {
        return -1;
    }

    memcpy(p->buf + p->len, contents, realsize);
    p->len += realsize;

    return nmemb;
}

HTTPFetcher::HTTPFetcher(string url, OffsetMgr *o)
    : BlockingBuffer(url, o), urlparser(url.c_str()), method(GET) {
    this->curl = curl_easy_init();
    if (this->curl) {
#if DEBUG_S3_CURL
        curl_easy_setopt(this->curl, CURLOPT_VERBOSE, 1L);
#endif
        // curl_easy_setopt(curl, CURLOPT_PROXY, "127.0.0.1:8080");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, WriterCallback);
        curl_easy_setopt(this->curl, CURLOPT_FORBID_REUSE, 1L);
        this->AddHeaderField(HOST, urlparser.Host());
    } else {
        S3ERROR("Failed to create curl instance, no enough memory?");
    }
}

HTTPFetcher::~HTTPFetcher() {
    if (this->curl) curl_easy_cleanup(this->curl);
}

bool HTTPFetcher::SetMethod(Method m) {
    this->method = m;
    return true;
}

bool HTTPFetcher::AddHeaderField(HeaderField f, string v) {
    if (v == "") {
        S3INFO("Skip adding empty field for %s", GetFieldString(f));
        return false;
    }
    return this->headers.Add(f, v);
}

// buffer size should be at least len
// read len data from offest
// returning -1 mearns error, pls don't set chunksize to uint64_t(-1) for now
uint64_t HTTPFetcher::fetchdata(uint64_t offset, char *data, uint64_t len) {
    if (len == 0) return 0;
    if (!this->curl) {
        S3ERROR("Can't fetch data without curl instance");
        return -1;
    }

    int retry_time = 3;
    Bufinfo bi;
    CURL *curl_handle = this->curl;
    struct curl_slist *chunk;
    char rangebuf[128];
    long respcode;

    while (retry_time--) {
        // "Don't call cleanup() if you intend to transfer more files, re-using
        // handles is a key to good performance with libcurl."
        if (retry_time != 2)  // sleep if retry
            usleep(3 * 1000 * 1000);

        bi.buf = data;
        bi.maxsize = len;
        bi.len = 0;

        curl_easy_setopt(curl_handle, CURLOPT_URL, this->sourceurl.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&bi);

        // consider low speed as timeout
        curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT,
                         s3ext_low_speed_limit);
        curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME,
                         s3ext_low_speed_time);

        memset(rangebuf, 0, 128);
        snprintf(rangebuf, 128, "bytes=%" PRIu64 "-%" PRIu64, offset,
                 offset + len - 1);
        this->AddHeaderField(RANGE, rangebuf);
        this->AddHeaderField(X_AMZ_CONTENT_SHA256, "UNSIGNED-PAYLOAD");
        if (!this->processheader()) {
            S3ERROR("Failed to sign while fetching data, retry");
            continue;
        }

        chunk = this->headers.GetList();
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        CURLcode res = curl_easy_perform(curl_handle);

        if (res == CURLE_WRITE_ERROR) {
            S3INFO("Curl downloading is interrupted by GPDB");
            bi.len = -1;
            break;
        }

        if (res == CURLE_OPERATION_TIMEDOUT) {
            S3WARN("Net speed is too slow, retry");
            bi.len = -1;
            continue;
        }

        if (res != CURLE_OK) {
            S3ERROR("curl_easy_perform() failed: %s, retry",
                    curl_easy_strerror(res));
            bi.len = -1;
            continue;
        } else {
            curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &respcode);
            S3DEBUG("Fetched %llu, %llu - %llu, response code is %ld", len,
                    offset, offset + len - 1, respcode);

            if ((respcode != 200) && (respcode != 206)) {
                S3ERROR("get %.*s, retry", (int)bi.len, data);
                bi.len = -1;
                continue;
            } else {
                break;
            }
        }
    }

    if (curl_handle) {
        this->curl = curl_handle;
    }
    curl_slist_free_all(chunk);
    return bi.len;
}

S3Fetcher::S3Fetcher(string url, string region, OffsetMgr *o,
                     const S3Credential &cred)
    : HTTPFetcher(url, o) {
    this->cred = cred;
    this->region = region;
}

bool S3Fetcher::processheader() {
    return SignRequestV4("GET", &this->headers, this->region,
                         this->urlparser.Path(), "", this->cred);
}

// CreateBucketContentItem
BucketContent::~BucketContent() {}

BucketContent::BucketContent() : key(""), size(0) {}

BucketContent *CreateBucketContentItem(string key, uint64_t size) {
    if (key == "") return NULL;

    BucketContent *ret = new BucketContent();
    if (!ret) {
        S3ERROR("Can't create bucket list, no enough memory?");
        return NULL;
    }
    ret->key = key;
    ret->size = size;
    return ret;
}

// require curl 7.17 higher
// http://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketGET.html
xmlParserCtxtPtr DoGetXML(string region, string bucket, string url,
                          string prefix, const S3Credential &cred,
                          string marker) {
    stringstream host;
    host << "s3-" << region << ".amazonaws.com";

    CURL *curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if DEBUG_S3_CURL
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    } else {
        S3ERROR("Can't create curl instance, no enough memory?");
        return NULL;
    }

    XMLInfo xml;
    xml.ctxt = NULL;

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&xml);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ParserCallback);

    HeaderContent *header = new HeaderContent();
    if (!header) {
        S3ERROR("Can allocate memory for header");
        return NULL;
    }

    header->Add(HOST, host.str());
    UrlParser p(url.c_str());
    header->Add(X_AMZ_CONTENT_SHA256, "UNSIGNED-PAYLOAD");

    std::stringstream query;
    if (marker != "") {
        query << "marker=" << marker << "&";
    }
    query << "prefix=" << prefix;

    if (!SignRequestV4("GET", header, region, p.Path(), query.str(), cred)) {
        S3ERROR("Failed to sign in DoGetXML()");
        delete header;
        return NULL;
    }

    struct curl_slist *chunk = header->GetList();

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        S3ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        if (xml.ctxt) {
            xmlDocPtr doc = xml.ctxt->myDoc;
            xmlFreeParserCtxt(xml.ctxt);
            xmlFreeDoc(doc);
            xml.ctxt = NULL;
        }
    } else {
        if (xml.ctxt) {
            xmlParseChunk(xml.ctxt, "", 0, 1);
        } else {
            S3ERROR("XML is downloaded but failed to be parsed");
        }
    }
    curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);
    delete header;
    return xml.ctxt;
}

static bool extractContent(ListBucketResult *result, xmlNode *root_element,
                           string &marker) {
    if (!result || !root_element) {
        return false;
    }

    xmlNodePtr cur;
    bool is_truncated = false;
    char *content = NULL;
    char *key = NULL;
    char *key_size = NULL;

    cur = root_element->xmlChildrenNode;
    while (cur != NULL) {
        if (key) {
            xmlFree(key);
            key = NULL;
        }

        if (!xmlStrcmp(cur->name, (const xmlChar *)"IsTruncated")) {
            content = (char *)xmlNodeGetContent(cur);
            if (content) {
                if (!strncmp(content, "true", 4)) {
                    is_truncated = true;
                }
                xmlFree(content);
            }
        }

        if (!xmlStrcmp(cur->name, (const xmlChar *)"Name")) {
            content = (char *)xmlNodeGetContent(cur);
            if (content) {
                result->Name = content;
                xmlFree(content);
            }
        }

        if (!xmlStrcmp(cur->name, (const xmlChar *)"Prefix")) {
            content = (char *)xmlNodeGetContent(cur);
            if (content) {
                result->Prefix = content;
                xmlFree(content);
                // content is not used anymore in this loop
                content = NULL;
            }
        }

        if (!xmlStrcmp(cur->name, (const xmlChar *)"Contents")) {
            xmlNodePtr contNode = cur->xmlChildrenNode;
            uint64_t size = 0;

            while (contNode != NULL) {
                // no memleak here, every content has only one Key/Size node
                if (!xmlStrcmp(contNode->name, (const xmlChar *)"Key")) {
                    key = (char *)xmlNodeGetContent(contNode);
                }
                if (!xmlStrcmp(contNode->name, (const xmlChar *)"Size")) {
                    key_size = (char *)xmlNodeGetContent(contNode);
                    // Size of S3 file is a natural number, don't worry
                    size = (uint64_t)atoll((const char *)key_size);
                }
                contNode = contNode->next;
            }

            if (key) {
                if (size > 0) {  // skip empty item
                    BucketContent *item = CreateBucketContentItem(key, size);
                    if (item) {
                        result->contents.push_back(item);
                    } else {
                        S3ERROR("Faild to create item for %s", key);
                    }
                } else {
                    S3INFO("Size of %s is " PRIu64 ", skip it", key, size);
                }
            }

            if (key_size) {
                xmlFree(key_size);
                key_size = NULL;
            }
        }

        cur = cur->next;
    }

    marker = (is_truncated && key) ? key : "";

    if (key) {
        xmlFree(key);
    }

    return true;
}

ListBucketResult *ListBucket(string schema, string region, string bucket,
                             string prefix, const S3Credential &cred) {
    string marker = "";

    stringstream host;
    host << "s3-" << region << ".amazonaws.com";

    S3DEBUG("Host url is %s", host.str().c_str());

    ListBucketResult *result = new ListBucketResult();
    if (!result) {
        S3ERROR("Failed to allocate bucket list result");
        return NULL;
    }

    stringstream url;
    xmlParserCtxtPtr xmlcontext = NULL;

    do {
        if (prefix != "") {
            url << schema << "://" << host.str() << "/" << bucket << "?";

            if (marker != "") {
                url << "marker=" << marker << "&";
            }

            url << "prefix=" << prefix;
        } else {
            url << schema << "://" << bucket << "." << host.str() << "?";

            if (marker != "") {
                url << "marker=" << marker;
            }
        }

        xmlcontext = DoGetXML(region, bucket, url.str(), prefix, cred, marker);
        if (!xmlcontext) {
            S3ERROR("Failed to list bucket for %s", url.str().c_str());
            delete result;
            return NULL;
        }

        xmlDocPtr doc = xmlcontext->myDoc;
        xmlNode *root_element = xmlDocGetRootElement(xmlcontext->myDoc);
        if (!root_element) {
            S3ERROR("Failed to parse returned xml of bucket list");
            delete result;
            xmlFreeParserCtxt(xmlcontext);
            xmlFreeDoc(doc);
            return NULL;
        }

        xmlNodePtr cur = root_element->xmlChildrenNode;
        while (cur != NULL) {
            if (!xmlStrcmp(cur->name, (const xmlChar *)"Code")) {
                char *content = (char *)xmlNodeGetContent(cur);
                if (content) {
                    S3ERROR("Server returns error \"%s\"", content);
                    xmlFree(content);
                }
                break;
            }

            cur = cur->next;
        }

        if (!extractContent(result, root_element, marker)) {
            S3ERROR("Failed to extract key from bucket list");
            delete result;
            xmlFreeParserCtxt(xmlcontext);
            xmlFreeDoc(doc);
            return NULL;
        }

        // clear url
        url.str("");

        // always cleanup
        xmlFreeParserCtxt(xmlcontext);
        xmlFreeDoc(doc);
        xmlcontext = NULL;
    } while (marker != "");

    return result;
}

ListBucketResult::~ListBucketResult() {
    vector<BucketContent *>::iterator i;
    for (i = this->contents.begin(); i != this->contents.end(); i++) {
        delete *i;
    }
}

ListBucketResult *ListBucket_FakeHTTP(string host, string bucket) {
    std::stringstream sstr;
    sstr << "http://" << host << "/" << bucket;

    CURL *curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, sstr.str().c_str());
#if DEBUG_S3_CURL
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
        // curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    } else {
        return NULL;
    }

    XMLInfo xml;
    xml.ctxt = NULL;

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&xml);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ParserCallback);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        S3ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        return NULL;
    }
    xmlParseChunk(xml.ctxt, "", 0, 1);
    if (!xml.ctxt) {
        S3ERROR("xmlParseChunk() failed");
        return NULL;
    }

    xmlDocPtr doc = xml.ctxt->myDoc;
    xmlNode *root_element = xmlDocGetRootElement(xml.ctxt->myDoc);
    if (!root_element) {
        S3ERROR("Failed to create xml node");
        return NULL;
    }

    ListBucketResult *result = new ListBucketResult();

    if (!result) {
        xmlFreeParserCtxt(xml.ctxt);
        xmlFreeDoc(doc);
        return NULL;
    }

    string marker = "";
    if (!extractContent(result, root_element, marker)) {
        delete result;
        xmlFreeParserCtxt(xml.ctxt);
        xmlFreeDoc(doc);
        return NULL;
    }

    /* always cleanup */
    xmlFreeParserCtxt(xml.ctxt);
    xmlFreeDoc(doc);
    return result;
}
