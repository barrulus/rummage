#pragma once

// Versioned, allocation-safe boundary: callers free every non-null response with
// rummage_photos_free from the same loaded module. No C++ types cross the boundary.
#ifdef __cplusplus
extern "C" {
#endif
int rummage_photos_version(void);
char* rummage_photos_request(const char* catalog_path, const char* request_json);
void rummage_photos_free(char* response);
#ifdef __cplusplus
}
#endif
