# STA2ETH Example

## Overview

This example demonstrates **pure Layer 2 (L2) packet forwarding** between WiFi Station (STA) mode and Ethernet. The ESP32 acts as a transparent bridge:
- PC connects via Ethernet to the ESP32
- ESP32 connects as a WiFi client (STA) to an access point (SSID: "xinxin", Password: "19840226")
- Packets are forwarded bidirectionally between Ethernet and WiFi **without IP processing**

## ⚠️ Important: L2 Bridge Mode

**This is a TRUE Layer 2 bridge:**
- ESP32 does **NOT** get an IP address on the WiFi STA interface
- ESP32 does **NOT** process packets at IP layer
- ESP32 only forwards raw Ethernet frames between WiFi and Ethernet
- No DHCP client runs on WiFi STA
- No IP stack involvement - pure MAC layer forwarding

## Architecture

```
[PC] <--Ethernet--> [ESP32 L2 Bridge] <--WiFi--> [AP Router "xinxin"]
     (Layer 2 only - No IP processing on ESP32)
```

### Packet Flow

1. **Ethernet to WiFi (pkt_eth2wifi)**: 
   - When packets arrive from PC via Ethernet
   - They are directly forwarded to WiFi STA interface
   - Uses `esp_wifi_internal_tx(WIFI_IF_STA, ...)`

2. **WiFi to Ethernet (pkt_wifi2eth)**:
   - When packets arrive from WiFi AP via STA interface
   - They are queued in a flow control queue
   - Worker task (`wifi2eth_flow_control_task`) forwards them to Ethernet
   - Uses `esp_eth_transmit(...)`

## Configuration

### Hardcoded WiFi Credentials
All configuration is now hardcoded in the source code and `sdkconfig.defaults`:

**WiFi Settings (in code):**
- SSID: "xinxin"
- Password: "19840226"
- Auth mode: WPA2-PSK

**Ethernet GPIO Settings (in sdkconfig.defaults):**
- MDC GPIO: 31
- MDIO GPIO: 52
- PHY RST GPIO: 51

### Removed Kconfig
The `Kconfig.projbuild` file has been completely removed. All configurations that were previously in Kconfig are now:
- Either hardcoded in the source code (`ethernet_example_main.c`)
- Or set in `sdkconfig.defaults`

## MAC Address Handling

### MAC Address Strategy

In this sta2eth implementation, **the WiFi STA MAC address is used for the Ethernet interface**:

```c
// When WiFi STA connects to AP:
esp_wifi_get_mac(WIFI_IF_STA, s_wifi_mac);
esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, s_wifi_mac);
```

### Why This Approach?

1. **Consistency**: The same MAC address appears to both the WiFi AP and the PC connected via Ethernet
2. **Simplicity**: The WiFi AP sees one device, and packets destined for that MAC are properly routed
3. **Transparency**: From the AP's perspective, it's communicating with a single device

### MAC Address Flow

```
1. ESP32 WiFi STA connects to AP "xinxin"
2. WiFi STA gets assigned MAC address (e.g., AA:BB:CC:DD:EE:FF)
3. Same MAC is programmed into Ethernet interface
4. PC connected via Ethernet uses the same MAC address
5. AP sees only one device with one MAC address
```

## DHCP Configuration

### DHCP Client Behavior

**Important**: In this L2 bridge configuration:

1. **WiFi STA side**: 
   - ESP32 does **NOT** act as a DHCP client
   - ESP32 does **NOT** get an IP address
   - WiFi STA is only used for L2 packet forwarding
   - No IP_EVENT_STA_GOT_IP event is used
   - **Pure Layer 2 operation - no IP stack involvement**

2. **Ethernet side**:
   - **No DHCP server runs on Ethernet interface**
   - **No DHCP client runs on Ethernet interface**
   - Ethernet operates at **Layer 2 only** (data link layer)
   - Pure packet forwarding without IP layer awareness

### Network Layer Operation

The bridge operates purely at Layer 2 (MAC/Ethernet frame level):

```
Layer 3 (IP):      [PC] <----- transparent -----> [AP "xinxin"]
                           ESP32 does NOT get IP (L2 only!)
                           
Layer 2 (MAC):     [PC] <--[ESP32 Ethernet]--[ESP32 WiFi]--> [AP]
                           Same MAC on both interfaces
```

### IP Address Assignment

**How PC gets IP address:**

1. PC sends DHCP DISCOVER via Ethernet
2. ESP32 forwards it **transparently** to WiFi (to AP) - L2 forwarding only
3. AP's DHCP server responds with DHCP OFFER
4. ESP32 forwards response **transparently** back to PC via Ethernet
5. DHCP handshake completes transparently through the L2 bridge

**Result**: Only PC gets IP address from the DHCP server (running on AP "xinxin")
**ESP32 does NOT get an IP** - it operates purely as a Layer 2 bridge

### DHCP Packet Forwarding

The implementation forwards DHCP packets like any other Ethernet frames:
- DHCP packets from PC go through `pkt_eth2wifi` → forwarded to WiFi
- DHCP responses from AP go through `pkt_wifi2eth` → forwarded to Ethernet
- No special DHCP handling needed due to pure Layer 2 forwarding
- ESP32 does NOT participate in DHCP - only forwards packets

## Important Notes

### 1. Pure L2 Bridge Mode
**This is critical**: The ESP32 operates as a pure Layer 2 bridge:
- No IP address on WiFi STA interface
- No IP stack processing on WiFi or Ethernet
- No IP event handlers registered
- Only raw Ethernet frame forwarding
- This prevents packet interception at IP layer

### 2. Promiscuous Mode
Ethernet is set to promiscuous mode to receive all packets:
```c
bool eth_promiscuous = true;
esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous);
```

### 3. WiFi Internal APIs
The example uses internal WiFi APIs for **raw packet forwarding**:
- `esp_wifi_internal_tx()` - Direct packet transmission (bypasses IP stack)
- `esp_wifi_internal_reg_rxcb()` - Register raw receive callback (Layer 2)
- `esp_wifi_internal_free_rx_buffer()` - Free WiFi RX buffer

These APIs ensure packets are forwarded at Layer 2 without IP processing.

### 4. Flow Control
A queue-based flow control mechanism handles speed differences between WiFi and Ethernet:
- Queue size: 40 packets
- Timeout: 100ms
- Handles WiFi being slower than Ethernet

### 5. Connection Sequence
```
1. System init (NVS, event loop)
2. Flow control task created
3. WiFi initialized and started (connects to "xinxin")
4. Ethernet initialized and started
5. WiFi connects to AP (L2 only - no IP obtained)
6. MAC address synchronized (WiFi MAC → Ethernet)
7. L2 bridge established - packet forwarding begins
8. PC can now obtain IP from AP via transparent DHCP forwarding
```

## Building and Flashing

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## Expected Behavior

1. **WiFi Connection**:
   - ESP32 connects to AP "xinxin" (L2 only)
   - ESP32 does **NOT** get an IP address (pure L2 bridge)
   - Log shows: "Wi-Fi STA connected to AP (L2 Bridge Mode - No IP)"
   - Log shows: "L2 Bridge established: WiFi MAC xx:xx:xx:xx:xx:xx set to Ethernet"

2. **Ethernet Connection**:
   - PC connects via Ethernet cable
   - Log shows: "Ethernet Link Up"

3. **Packet Forwarding**:
   - PC can access network through WiFi AP
   - PC gets IP from AP's DHCP server (transparently forwarded)
   - ESP32 operates only as L2 bridge - no IP processing

## Troubleshooting

### Issue: WiFi not connecting
- Verify AP "xinxin" is available
- Check password "19840226" is correct
- Ensure AP is on 2.4GHz band (ESP32 doesn't support 5GHz)

### Issue: Ethernet not working
- Check Ethernet cable connection
- Verify GPIO pins (31, 52, 51) match your hardware
- Check PHY reset GPIO is correct

### Issue: PC not getting IP
- Ensure WiFi connection is established first
- Check AP's DHCP server is enabled
- Verify packet forwarding is working (check logs for "WiFi send packet failed" or "Ethernet send packet failed")

## Technical References

- [ESP-IDF WiFi Station Example](https://github.com/espressif/esp-idf/tree/master/examples/wifi/getting_started/station)
- [ESP-IDF Ethernet Examples](https://github.com/espressif/esp-idf/tree/master/examples/ethernet)
- [ESP-IDF sta2eth Example](https://github.com/espressif/esp-idf/tree/master/examples/network/sta2eth)
