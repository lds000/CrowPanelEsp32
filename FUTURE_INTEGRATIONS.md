# Future integrations

Integrations belong on the controller hub/Pi, not in ESP32 firmware. The panel
should remain a bounded display and command client; the hub has the memory,
network stack, audit trail, provider SDKs, upgrade cadence, and policy context.

## Home Assistant and MQTT

Prefer MQTT Discovery with stable unique IDs for:

- Hub and panel availability/data age
- Zone state and active run
- Measured flow and pressure
- Soil moisture and rainfall/rain skip
- Lightning proximity
- Schedule summary and next runs
- Leak, dry-run and pressure alarms

Home Assistant can then provide mobile alerts, voice entry points, dashboards,
and automations without coupling ESP32 firmware to a particular dashboard. Start
with read-only sensors. Add command entities only after authentication,
availability and deterministic safety checks are proven.

## Optional LLM service

Keep the hub provider-neutral and evaluate models on real irrigation questions,
cost, latency and failure modes. As of 2026-07-17, a sensible optional OpenAI
routing experiment is GPT-5.6 Terra for normal explanations/reports, GPT-5.6 Sol
for difficult cross-sensor incident analysis, and GPT-5.6 Luna for inexpensive
classification/summaries. Existing Anthropic models may perform equally well or
better on this workload; use an evaluation set instead of changing providers on
vibes alone.

The LLM must never actuate irrigation directly:

```text
question -> model proposes typed command -> deterministic hub policy validates
         -> user confirms -> authenticated/idempotent backend executes -> audit
```

The policy layer must validate zone ID, duration, data freshness, rain,
lightning, measured flow/pressure, concurrent runs, authorization and hard
limits. STOP remains deterministic and does not wait for a model. Send only the
minimum sensor/history window and no device credentials to a provider.

Voice belongs in the hub, browser, phone, or Home Assistant. The panel can show
the result, but should not absorb a realtime audio/LLM stack merely because its
flash partition was looking insufficiently terrified.

Useful references:

- <https://www.home-assistant.io/integrations/mqtt>
- <https://developers.openai.com/api/docs/models>
- <https://developers.openai.com/api/docs/guides/function-calling>
- <https://developers.openai.com/api/docs/guides/realtime>
