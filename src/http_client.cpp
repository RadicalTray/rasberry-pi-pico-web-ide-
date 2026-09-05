// #include <Arduino.h>
//
// struct Response {
// 	int id;
// 	String body;
// };
//
// std::vector<Response> responses;
// std::vector<HttpClient> pending_requests;
// uint32_t currentIndex;
//
// const auto endpoint = "/v1/chat/completions";
// const auto contentType = "application/json";
//
// int sendAgent(const String &prompt) {
//     auto req = new HttpClient(net, "vaam01.3bbddns.com", 43954);
//     err = req->post(endpoint, contentType, prompt.toJson()); // TODO
//     if (err) {
//       Serial.printf("Failed to send http post: %d\n", err);
//       return err;
//     }
// }
//
// void oiaednroieanrd() {
//     const auto endpoint = "/v1/chat/completions";
//     const auto contentType = "application/json";
//     const auto hello = R"---({"model": "/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4", "messages": [{"role": "user", "content": "Hello!"}], "stream": false})---";
//     const auto meow = R"---({"model": "/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4", "messages": [{"role": "user", "content": "Meow!"}], "stream": false})---";
//     int err = 0;
//
//     HttpClient req1(net, "vaam01.3bbddns.com", 43954);
//     err = req1.post(endpoint, contentType, hello);
//     if (err) {
//       Serial.printf("Failed to send http post: %d\n", err);
//       while (true) {}
//     }
//
//     Serial.print("Waiting 10 seconds...\n");
//     delay(10000);
//
//     HttpClient req2(net, "vaam01.3bbddns.com", 43954);
//     err = req2.post(endpoint, contentType, meow);
//     if (err) {
//       Serial.printf("Failed to send http post: %d\n", err);
//       while (true) {}
//     }
//
//     Serial.print("--- READING 2ND RESPONSE ---\n");
//     readResponse(req2);
//     delay(10000);
//     Serial.print("--- READING 1ST RESPONSE ---\n");
//     readResponse(req1);
//
//     Serial.print("Stopping...\n");
//     while (true) {}
//
// }
//
// void loopHttpClients() {
// }
