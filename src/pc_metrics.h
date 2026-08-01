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

// Value history for the sparkline and chart faces, sampled once per received
// packet (~3 s companion cadence -> 60 samples = ~3 min window).
//
// Keyed by METRIC ID, not by gauge slot. The samples belong to the metric, not
// to the grid position it happens to occupy, so a slot rebind or a metric page
// switch no longer throws the ring away. That is what makes page rotation
// usable: rotating every few seconds under the old slot-keyed rings reset every
// chart on every switch, so no chart ever filled.
//
// Every metric the packet carries is sampled, not only the bound ones, so a
// chart is already warm the moment the user binds a metric or rotates to a page
// that shows it. Costs ~8 KB of static RAM.
#define PC_HISTORY_LEN 60

struct SlotHistory {
  uint8_t metricId;   // metric id the samples belong to; 0 for the empty ring
  uint8_t count;      // valid samples (0..PC_HISTORY_LEN)
  uint8_t head;       // next write index
  float   samples[PC_HISTORY_LEN];
};

// Indexed by metric id. Index 0 is the permanently empty "nothing bound" ring,
// so an unbound slot charts nothing without needing a branch at each call site.
extern SlotHistory pcHistoryById[MAX_METRICS + 1];

// History for whatever the given slot of the ACTIVE metric page is bound to.
const SlotHistory& pcHistoryForSlot(uint8_t slotIdx);

// Sample oldest->newest: i in [0, count).
static inline float pcHistoryAt(const SlotHistory& h, uint8_t i) {
  return h.samples[(uint8_t)((h.head + PC_HISTORY_LEN - h.count + i) % PC_HISTORY_LEN)];
}

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
