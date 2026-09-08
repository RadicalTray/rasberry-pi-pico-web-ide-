#include <Arduino.h>
#include <HTTPClient.h>
#include <FreeRTOS.h>

#define ERR_UNINITIALIZED -1000
#define ERR_RESPONSE_FULL -1001
#define ERR_QUEUE_FULL    -1002
#define ERR_INVALID       -1003
#define ERR_WTF           -1067

#define MAX_INFLIGHT 16

struct Request {
    String url;
    String body;
    String contentType;
    int handle;
};

struct Response {
    SemaphoreHandle_t mutex;
    String text;
    int code = ERR_UNINITIALIZED;
    bool done = false;
    bool used = false; // set by httpPost(), unset by httpWait()

    void reset() {
        text = "";
        code = ERR_UNINITIALIZED;
        done = false;
        used = false;
    }
};

static TaskHandle_t taskHandle = NULL;
static QueueHandle_t requestQueue = nullptr;
static Response responses[MAX_INFLIGHT];

static void httpTask(void *params) {
    Request *req;
    while (true) {
        if (xQueueReceive(requestQueue, &req, portMAX_DELAY) == pdTRUE) {
            HTTPClient http;
            String text;
            int httpCode;
            if (http.begin(req->url)) {
                http.addHeader("Content-Type", req->contentType);
                http.setTimeout(30 * 1000);

                httpCode = http.POST(req->body);
                if (httpCode > 0) {
                    text = http.getString();
                } else {
                    text = http.errorToString(httpCode);
                }
                http.end();
            } else {
                httpCode = ERR_UNINITIALIZED;
                text = "can't connect to " + req->url;
            }

            xSemaphoreTake(responses[req->handle].mutex, portMAX_DELAY);
            if (responses[req->handle].used) {
                responses[req->handle].code = httpCode;
                responses[req->handle].text = text;
                responses[req->handle].done = true;
            } // used = false, means httpClear() was called
            xSemaphoreGive(responses[req->handle].mutex);
            delete req;
        }
    }
}

void httpInit() {
    requestQueue = xQueueCreate(MAX_INFLIGHT, sizeof(Request*));

    for (int i = 0; i < MAX_INFLIGHT; i++) {
        responses[i].mutex = xSemaphoreCreateMutex();
        responses[i].reset();
    }

    xTaskCreate(httpTask, "httpTask", 4096, nullptr, 1, &taskHandle);
}

void httpClear() {
    xQueueReset(requestQueue);
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        xSemaphoreTake(responses[i].mutex, portMAX_DELAY);
        responses[i].reset();
        xSemaphoreGive(responses[i].mutex);
    }
}

int httpPost(const String &url, const String &contentType, const String &body) {
    if (!requestQueue)
        return ERR_UNINITIALIZED;

    int handle = -1;
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        xSemaphoreTake(responses[i].mutex, portMAX_DELAY);
        if (!responses[i].used) {
            responses[i].reset();
            responses[i].used = true;
            handle = i;
        }
        xSemaphoreGive(responses[i].mutex);
        if (handle >= 0) // would be nicer if c had fkin defer
            break;
    }
    if (handle < 0)
        return ERR_RESPONSE_FULL;

    Request *req = new Request{
        .url = url,
        .body = body,
        .contentType = contentType,
        .handle = handle,
    };

    if (xQueueSend(requestQueue, &req, 0) != pdTRUE) {
        delete req;
        return ERR_QUEUE_FULL;
    }

    return handle;
}

int httpWait(int handle, int *code, String &text) {
    if (!requestQueue) return ERR_UNINITIALIZED;
    if (handle < 0 || handle >= MAX_INFLIGHT) return ERR_INVALID;

    while (true) {
        xSemaphoreTake(responses[handle].mutex, portMAX_DELAY);
        if (!responses[handle].used) {
            // used = false, means httpClear() was called
            xSemaphoreGive(responses[handle].mutex);
            return ERR_INVALID;
        } else if (responses[handle].done) {
            *code = responses[handle].code;
            text = responses[handle].text;
            responses[handle].reset();
            xSemaphoreGive(responses[handle].mutex);
            return 0;
        } else {
            xSemaphoreGive(responses[handle].mutex);
        }
    }

    return ERR_WTF; // unreachable
}
