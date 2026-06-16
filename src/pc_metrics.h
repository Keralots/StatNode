#ifndef PC_METRICS_H
#define PC_METRICS_H

#include <Arduino.h>
#include "config.h"

// One metric as pushed by the PC companion (V2.1 protocol). Slim model: the
// OLED project's layout fields (position/barPosition/companionId/...) are gone
// because the color gauge UI binds metrics to gauge slots in settings, not in
// the packet.
struct PcMetric {
  uint8_t id;            // 1-based stable id; the gauge mapping binds by this
  char    name[16];      // short sensor name from the companion ("CPU","GPUT")
  char    unit[8];       // "%", "C", "W", "RPM", "MB", ...
  float   value;
};

// Latest decoded packet + liveness.
struct PcMetricData {
  PcMetric metrics[MAX_METRICS];
  uint8_t  count;
  char     timestamp[6];   // "HH:MM" from the companion
  uint8_t  status;         // PC_STATUS_* (LHM health reported by the companion)
  bool     online;         // a fresh packet arrived within PC_STALE_TIMEOUT_MS
  uint32_t lastReceived;   // millis() of the last valid packet
};

extern PcMetricData pcData;

// Start the UDP listener on UDP_PORT.
void pcMetricsBegin();
// Service the socket: parse any pending packet, refresh the online flag.
// Returns true if a new packet was parsed this call.
bool pcMetricsHandle();
// Re-bind the socket after a WiFi reconnect (old fd goes stale).
void pcMetricsRestartUdp();

// Lookups for the renderer / gauge mapping.
const PcMetric* pcMetricFindById(uint8_t id);
const PcMetric* pcMetricFindByName(const char* name);

#endif // PC_METRICS_H
