#ifndef MCPBRIDGE_H
#define MCPBRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

void mcpbridge_init(int port);
void mcpbridge_shutdown(void);
void mcpbridge_drain(void);

#ifdef __cplusplus
}
#endif

#endif
