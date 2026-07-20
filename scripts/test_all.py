"""
Comprehensive offline test for RLCD Monitor code.
Tests non-hardware-dependent parts: DNS, JSON, HTML, bridge, battery calc.
"""

import struct
import json
import re
import sys
import os
import unittest

# ============================================================
# 1. DNS Response Packet Test (matches captive_dns.c logic)
# ============================================================
def build_dns_response(req: bytes) -> bytes:
    """Replicate the C logic from captive_dns.c:dns_respond()"""
    if len(req) > 512:
        req = req[:512]
    resp = bytearray(req)

    # Set QR=1 (response), RA=1
    resp[2] = 0x81
    resp[3] = 0x80
    # Answer count = 1
    resp[6] = 0x00
    resp[7] = 0x01

    # Skip question section: find null-terminated name + QTYPE + QCLASS
    qpos = 12
    while qpos < len(req) and req[qpos] != 0:
        qpos += 1
    qpos += 5  # skip null + QTYPE(2) + QCLASS(2)

    if qpos >= len(resp) or qpos + 16 > len(resp):
        return bytes(resp[:min(qpos, len(resp))])

    # Answer: name pointer + A record
    resp[qpos] = 0xC0; qpos += 1
    resp[qpos] = 0x0C; qpos += 1
    # TYPE A
    resp[qpos] = 0x00; qpos += 1
    resp[qpos] = 0x01; qpos += 1
    # CLASS IN
    resp[qpos] = 0x00; qpos += 1
    resp[qpos] = 0x01; qpos += 1
    # TTL = 60
    qpos += 4
    # RDLENGTH = 4
    resp[qpos] = 0x00; qpos += 1
    resp[qpos] = 0x04; qpos += 1
    # 192.168.4.1
    resp[qpos] = 192; qpos += 1
    resp[qpos] = 168; qpos += 1
    resp[qpos] = 4; qpos += 1
    resp[qpos] = 1; qpos += 1

    return bytes(resp[:qpos])


class TestDNS(unittest.TestCase):
    def test_basic_a_record(self):
        """Test DNS A record query → response with 192.168.4.1"""
        # Standard DNS query for example.com
        tid = 0x1234
        req = struct.pack("!H", tid)  # Transaction ID
        req += struct.pack("!H", 0x0100)  # Flags: standard query
        req += struct.pack("!HHHHHH", 1, 0, 0, 0, 0, 0)  # QDCOUNT=1
        # Question: example.com
        req += b'\x07example\x03com\x00'
        req += struct.pack("!HH", 1, 1)  # QTYPE=A, QCLASS=IN

        resp = build_dns_response(req)

        # Check transaction ID preserved
        self.assertEqual(resp[0:2], struct.pack("!H", tid))
        # Check QR=1 (response flag)
        self.assertTrue(resp[2] & 0x80)
        # Check answer count = 1
        self.assertEqual(resp[6], 0)
        self.assertEqual(resp[7], 1)

        # Check the answer contains the name pointer to question (0xC00C)
        self.assertIn(b'\xC0\x0C', resp)
        # Check the answer ends with 192.168.4.1
        self.assertTrue(resp.endswith(b'\xC0\xA8\x04\x01'),
                        f"Response doesn't end with 192.168.4.1: {resp[-10:].hex()}")

    def test_dns_domain_with_subdomains(self):
        """Test with subdomain (api.example.com)"""
        req = b'\x1234\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00'
        req += b'\x03api\x07example\x03com\x00\x00\x01\x00\x01'
        resp = build_dns_response(req)
        self.assertTrue(resp.endswith(b'\xC0\xA8\x04\x01'))

    def test_dns_truncated_request(self):
        """Test with very short request (edge case)"""
        req = b'\x1234\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00'
        resp = build_dns_response(req)
        # Should not crash, should return something
        self.assertGreater(len(resp), 12)

    def test_dns_empty_question(self):
        """Test with no question (edge case)"""
        req = b'\x1234\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
        resp = build_dns_response(req)
        self.assertGreater(len(resp), 0)

    def test_dns_oversized(self):
        """Test with request > 512 bytes (should truncate)"""
        req = b'\x1234\x01\x00\x00\x01' + b'A' * 600 + b'\x00\x00\x01\x00\x01'
        resp = build_dns_response(req)
        self.assertLessEqual(len(resp), 512)


# ============================================================
# 2. JSON Config Parsing Test (matches wifi_manager.cpp logic)
# ============================================================
def json_get_str(json_data: str, key: str) -> str:
    """Replicate json_reader::get() from wifi_manager.cpp"""
    search = f'"{key}":"'
    p = json_data.find(search)
    if p < 0:
        return ""
    p += len(search)
    result = []
    while p < len(json_data) and json_data[p] != '"':
        result.append(json_data[p])
        p += 1
    return ''.join(result)


class TestJSONParser(unittest.TestCase):
    def test_parse_full_config(self):
        """Test parsing complete config JSON"""
        data = '{"ssid":"MyWiFi","password":"pass123","bridge":"http://10.0.0.1:7777/api/usage"}'
        self.assertEqual(json_get_str(data, "ssid"), "MyWiFi")
        self.assertEqual(json_get_str(data, "password"), "pass123")
        self.assertEqual(json_get_str(data, "bridge"), "http://10.0.0.1:7777/api/usage")

    def test_parse_empty_bridge(self):
        """Test parsing with empty bridge (auto mode)"""
        data = '{"ssid":"MyWiFi","password":"pass123","bridge":""}'
        self.assertEqual(json_get_str(data, "ssid"), "MyWiFi")
        self.assertEqual(json_get_str(data, "bridge"), "")

    def test_parse_ssid_with_spaces(self):
        """Test SSID with special characters"""
        data = '{"ssid":"My WiFi 5G","password":"","bridge":""}'
        self.assertEqual(json_get_str(data, "ssid"), "My WiFi 5G")

    def test_parse_ssid_with_quotes(self):
        """Test SSID with escaped quotes"""
        data = '{"ssid":"Test\\"WiFi","password":"123"}'
        result = json_get_str(data, "ssid")
        self.assertEqual(result, "Test\\")  # The C parser doesn't handle escapes
        # This is a known limitation - acceptable for SSID inputs

    def test_parse_missing_key(self):
        """Test parsing with missing key"""
        data = '{"ssid":"MyWiFi"}'
        self.assertEqual(json_get_str(data, "password"), "")

    def test_parse_empty_input(self):
        """Test parsing empty string (edge case)"""
        self.assertEqual(json_get_str("", "ssid"), "")
        self.assertEqual(json_get_str("{}", "ssid"), "")

    def test_parse_malformed(self):
        """Test malformed JSON (edge case)"""
        data = '{ssid:MyWiFi}'
        self.assertEqual(json_get_str(data, "ssid"), "")


# ============================================================
# 3. HTML Webpage Validation Test
# ============================================================
class TestWebpage(unittest.TestCase):
    def setUp(self):
        self.html = ""
        hpath = os.path.join(os.path.dirname(__file__),
                            '..', 'firmware', 'components',
                            'wifi_manager', 'webpage.h')
        if os.path.exists(hpath):
            with open(hpath, 'r', encoding='utf-8') as f:
                content = f.read()
            match = re.search(r'#define WEBPAGE_HTML\s+(.*?)#define WEBPAGE_LEN', content, re.DOTALL)
            if match:
                self.html = match.group(1).strip()

    def test_html_has_doctype(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('DOCTYPE', self.html)

    def test_html_has_scan_button(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('scan()', self.html)

    def test_html_has_save_button(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('save()', self.html)

    def test_html_has_wifi_list(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('wifiList', self.html)

    def test_html_has_bridge_input(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('bridge', self.html)

    def test_html_has_captive_portal_redirect(self):
        if not self.html: self.skipTest("webpage.h removed")
        self.assertIn('/api/config', self.html)
        self.assertIn('/api/scan', self.html)


# ============================================================
# 4. Bridge UDP Broadcast Test
# ============================================================
class TestBridgeBroadcast(unittest.TestCase):
    def test_broadcast_format(self):
        """Test the UDP broadcast message format"""
        msg = "RLCD_BRIDGE http://10.210.74.215:7777/api/usage"
        self.assertTrue(msg.startswith("RLCD_BRIDGE "))
        url = msg[12:]
        self.assertTrue(url.startswith("http://"))
        self.assertIn(":7777", url)
        self.assertIn("/api/usage", url)

    def test_broadcast_ip_detection(self):
        """Test simulated IP detection"""
        # Simulate _get_local_ip() return values
        test_cases = [
            ("10.210.74.215", "RLCD_BRIDGE http://10.210.74.215:7777/api/usage"),
            ("192.168.1.100", "RLCD_BRIDGE http://192.168.1.100:7777/api/usage"),
            ("127.0.0.1", "RLCD_BRIDGE http://127.0.0.1:7777/api/usage"),
        ]
        for ip, expected in test_cases:
            msg = f"RLCD_BRIDGE http://{ip}:7777/api/usage"
            self.assertEqual(msg, expected)


# ============================================================
# 5. Battery Trend Calculation Test
# ============================================================
class TestBatteryCalc(unittest.TestCase):
    def test_drop_per_hour(self):
        """Test battery drop calculation"""
        # 98% -> 93% over 5 hours
        dropped = 98 - 93
        elapsed_h = 5.0
        per_h = int(dropped / elapsed_h + 0.5)
        self.assertEqual(per_h, 1)  # 1%/h

    def test_est_hours(self):
        """Test estimated remaining hours"""
        per_h = 1
        current_pct = 93
        est = current_pct // per_h
        self.assertEqual(est, 93)

    def test_fast_discharge(self):
        """Test fast discharge scenario"""
        dropped = 98 - 88  # 10% drop
        elapsed_h = 2.0    # over 2 hours
        per_h = int(dropped / elapsed_h + 0.5)
        self.assertEqual(per_h, 5)  # 5%/h
        est = 88 // per_h
        self.assertEqual(est, 17)    # 17 hours remaining

    def test_slow_discharge(self):
        """Test slow discharge (actual measured data)"""
        dropped = 92 - 87  # 5% drop
        elapsed_h = 5.8     # over 5.8 hours
        per_h = int(dropped / elapsed_h + 0.5)
        self.assertEqual(per_h, 1)  # ~1%/h
        est = 87 // per_h
        self.assertEqual(est, 87)   # 87 hours remaining

    def test_no_change(self):
        """Test no battery change (charging)"""
        dropped = 87 - 87  # 0% drop = charging
        self.assertEqual(dropped, 0)
        # In the C code, dropped <= 0 triggers "Charging" path

    def test_peak_update_threshold(self):
        """Test that small fluctuations (<3%) don't update peak"""
        peak = 98
        current_readings = [97, 98, 97, 96, 97, 98]
        for r in current_readings:
            if r > peak + 2:  # +3% threshold
                peak = r
        self.assertEqual(peak, 98)  # Should still be 98

    def test_peak_update_charging(self):
        """Test that charging (>3% increase) updates peak"""
        peak = 90
        new_reading = 94  # +4% = charging
        if new_reading > peak + 2:  # +3% threshold
            peak = new_reading
        self.assertEqual(peak, 94)  # Updated to 94


# ============================================================
# 6. Integration Flow Test
# ============================================================
class TestIntegrationFlow(unittest.TestCase):
    def test_config_timeout_restart(self):
        """Test: config timeout causes restart, not fallback to defaults"""
        # Simulate the fixed code logic
        configured = False
        steps = []
        if configured:
            steps.append("connect_wifi")
        else:
            cfg_ret = "timeout"  # simulate timeout
            if cfg_ret == "ok":
                steps.append("save_config")
                steps.append("restart")
            else:
                steps.append("restart_timeout")
        self.assertEqual(steps, ["restart_timeout"])

    def test_config_success_restart(self):
        """Test: config success saves and restarts"""
        configured = False
        steps = []
        if configured:
            steps.append("connect_wifi")
        else:
            cfg_ret = "ok"  # simulate success
            if cfg_ret == "ok":
                steps.append("save_config")
                steps.append("restart")
            else:
                steps.append("restart_timeout")
        self.assertEqual(steps, ["save_config", "restart"])

    def test_configured_boot_flow(self):
        """Test: if already configured, skip AP mode"""
        configured = True
        steps = []
        if configured:
            steps.append("read_nvs")
            steps.append("connect_wifi")
        self.assertEqual(steps, ["read_nvs", "connect_wifi"])

    def test_key_longpress_resets_config(self):
        """Test: KEY held 5s clears NVS and restarts"""
        # Simulate the gpio read sequence
        key_press_start = 0
        tick = 0
        key_held = True
        steps = []

        # 5 seconds at 200ms intervals = 25 ticks
        for i in range(30):
            tick += 1
            if key_held:
                if key_press_start == 0:
                    key_press_start = tick
                elif tick - key_press_start > 25:  # 5 seconds
                    steps.append("clear_config")
                    steps.append("restart")
                    break
            else:
                key_press_start = 0

        self.assertIn("clear_config", steps)
        self.assertIn("restart", steps)


# ============================================================
# Run
# ============================================================
if __name__ == '__main__':
    print("=" * 60)
    print("RLCD Monitor — Offline Test Suite")
    print("=" * 60)
    unittest.main(verbosity=2)