# Publishing sensor values to MQTT

The firmware subscribes to two topics (set in `include/config.h`):

```
homeassistant/sensor/temphumzolder_temperature/state
homeassistant/sensor/temphumzolder_humidity/state
```

You already have these working via `mqtt_statestream`. This file records that
config plus a more robust alternative, in case you want retained values.

## Option A — mqtt_statestream (what you're using)

In `configuration.yaml`:

```yaml
mqtt_statestream:
  base_topic: homeassistant
  publish_attributes: false
  publish_timestamps: false
  include:
    entities:
      - sensor.temphumzolder_temperature
      - sensor.temphumzolder_humidity
```

Restart Home Assistant (a YAML reload is not enough for this integration).

Caveat: statestream publishes only on a state change and does not retain, so
right after a broker/ESP restart the topic can be empty until the next update.
The firmware shows `--` until the first value arrives, so this is cosmetic.

## Option B — automation with retain (if the empty-after-restart gap bothers you)

This republishes on change, on HA start, and every 5 minutes, with `retain:
true`, so the display always has a value immediately. If you use this, point the
firmware topics at `home/xiao/temperature` and `home/xiao/humidity` instead.

```yaml
alias: Publish XIAO display values
mode: queued
triggers:
  - trigger: state
    entity_id: sensor.temphumzolder_temperature
    id: temp
  - trigger: state
    entity_id: sensor.temphumzolder_humidity
    id: hum
  - trigger: homeassistant
    event: start
    id: both
  - trigger: time_pattern
    minutes: "/5"
    id: both
actions:
  - if:
      - condition: template
        value_template: >
          {{ trigger.id in ['temp','both']
             and states('sensor.temphumzolder_temperature') | is_number }}
    then:
      - action: mqtt.publish
        data:
          topic: home/xiao/temperature
          payload: "{{ states('sensor.temphumzolder_temperature') | float | round(1) }}"
          retain: true
  - if:
      - condition: template
        value_template: >
          {{ trigger.id in ['hum','both']
             and states('sensor.temphumzolder_humidity') | is_number }}
    then:
      - action: mqtt.publish
        data:
          topic: home/xiao/humidity
          payload: "{{ states('sensor.temphumzolder_humidity') | float | round(0) }}"
          retain: true
```

## Verify from any machine on the LAN

```
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'homeassistant/sensor/temphumzolder_+/state' -v
# or, for Option B:
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'home/xiao/#' -v
```

## Watch the device's own logs

```
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'xiao/#' -v
```

You'll see connection info on `xiao/log` and `online`/`offline` on `xiao/status`.
