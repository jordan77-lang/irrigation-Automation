```markdown
# Device onboarding & verification

1. Verify time
   - Device must perform NTP sync before validating signature and interpreting event times.

2. Fetch schedule
   - GET: https://raw.githubusercontent.com/<owner>/irrigation-Automation/main/schedules/schedules.json
   - GET: https://raw.githubusercontent.com/<owner>/irrigation-Automation/main/schedules/schedules.json.sig

3. Verify signature (HMAC-SHA256)
   - Compute HMAC-SHA256(payload, SIGN_KEY).
   - Compare hex digest to the contents of schedules.json.sig.
   - If mismatch, do NOT execute any events.

4. Parse JSON
   - Find entry for your device_id.
   - Each event is:
     {
       "id": "...",
       "action": "open"|"close",
       "time": "YYYY-MM-DDTHH:MM:SSZ",
       "virtual_angle": 1620.0,
       "expected_duration_s": 60
     }

5. Virtual angles & multi-turn
   - virtual_angle is absolute degrees. Device must maintain `current_virtual_position` in nonvolatile storage.
   - Compute delta = target_virtual - current_virtual_position and execute motion to meet that absolute target.
   - After successful move, update persisted current_virtual_position to the target.

6. Polling cadence
   - Poll the JSON daily by default.
   - If an upcoming event is within 30 minutes, poll more frequently (e.g., every 30s).
   - Verify signature every time you download.

7. Failure handling
   - If move fails, retry up to N times, then alert (LED + server call or manual inspection).
```