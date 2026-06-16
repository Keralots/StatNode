#ifndef WEB_SERVER_H
#define WEB_SERVER_H

void initWebServer();
void handleWebServer();

#ifdef ENABLE_OTA_AUTO
bool        isOtaAutoInProgress();
int         getOtaAutoProgress();
const char* getOtaAutoStatus();
#else
inline bool        isOtaAutoInProgress() { return false; }
inline int         getOtaAutoProgress()  { return 0; }
inline const char* getOtaAutoStatus()    { return ""; }
#endif

#endif // WEB_SERVER_H
