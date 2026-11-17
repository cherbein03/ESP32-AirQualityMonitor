/**
 * Hubitat-compatible driver for ESP32-C6 Air Quality Monitor.
 * Pair the device, then run Configure. It exposes standard Zigbee measurements
 * plus TVOC/AQI using Zigbee Analog Input endpoints (4 and 5).
 */
metadata {
    definition(name: "ESP32-C6 Air Quality Monitor", namespace: "local", author: "Local") {
        capability "Sensor"
        capability "TemperatureMeasurement"
        capability "RelativeHumidityMeasurement"
        capability "CarbonDioxideMeasurement"
        capability "ParticulateMatter"
        capability "Refresh"
        attribute "tvoc", "number"
        attribute "aqi", "number"
        attribute "zigbeeStatus", "string"
    }
    preferences {
        input name: "reportInterval", type: "enum", title: "Reporting interval", options: ["10":"10 seconds", "30":"30 seconds", "60":"1 minute", "300":"5 minutes", "900":"15 minutes"], defaultValue: "60", required: true
        input name: "temperatureUnit", type: "enum", title: "Temperature display", options: ["C":"Celsius", "F":"Fahrenheit"], defaultValue: "C", required: true
    }
}

def installed() { configure() }
def updated() { configure() }

def configure() {
    Integer interval = (settings.reportInterval ?: "60") as Integer
    // Values are stored/reported in standard Zigbee units (temperature °C).
    // Hubitat's preferred scale is applied when creating the temperature event.
    List cmds = []
    // Configure Reporting arguments: cluster, attribute, ZCL datatype,
    // min seconds, max seconds, reportable change, destination endpoint.
    cmds += zigbee.configureReporting(0x0402, 0x0000, 0x29, 0, interval, 20, [destEndpoint: 1])
    cmds += zigbee.configureReporting(0x0405, 0x0000, 0x21, 0, interval, 100, [destEndpoint: 1])
    cmds += zigbee.configureReporting(0x042A, 0x0000, 0x21, 0, interval, 1, [destEndpoint: 2])
    cmds += zigbee.configureReporting(0x040D, 0x0000, 0x21, 0, interval, 25, [destEndpoint: 3])
    cmds += zigbee.configureReporting(0x000C, 0x0055, 0x39, 0, interval, 10.0f, [destEndpoint: 4])
    cmds += zigbee.configureReporting(0x000C, 0x0055, 0x39, 0, interval, 1.0f, [destEndpoint: 5])
    cmds += zigbee.readAttribute(0x0402, 0x0000, [destEndpoint: 1])
    cmds += zigbee.readAttribute(0x0405, 0x0000, [destEndpoint: 1])
    cmds += zigbee.readAttribute(0x042A, 0x0000, [destEndpoint: 2])
    cmds += zigbee.readAttribute(0x040D, 0x0000, [destEndpoint: 3])
    cmds += zigbee.readAttribute(0x000C, 0x0055, [destEndpoint: 4])
    cmds += zigbee.readAttribute(0x000C, 0x0055, [destEndpoint: 5])
    cmds
}

def refresh() { configure() }

def parse(String description) {
    Map desc = zigbee.parseDescriptionAsMap(description)
    if (!desc) return
    Integer ep = desc.endpoint ? Integer.parseInt(desc.endpoint, 16) : 1
    Integer cluster = desc.clusterInt ?: (desc.cluster ? Integer.parseInt(desc.cluster, 16) : 0)
    Integer attr = desc.attrInt ?: (desc.attrId ? Integer.parseInt(desc.attrId, 16) : -1)
    String raw = desc.value
    if (raw == null) return
    BigInteger n = new BigInteger(raw, 16)
    if (n.testBit(15)) n -= (BigInteger.ONE << 16)

    if (ep == 1 && cluster == 0x0402 && attr == 0x0000) {
        BigDecimal c = new BigDecimal(n).divide(new BigDecimal(100))
        BigDecimal shown = settings.temperatureUnit == "F" ? c * 9 / 5 + 32 : c
        sendEvent(name: "temperature", value: shown.setScale(1, BigDecimal.ROUND_HALF_UP), unit: settings.temperatureUnit ?: "C")
    } else if (ep == 1 && cluster == 0x0405 && attr == 0x0000) {
        sendEvent(name: "humidity", value: new BigDecimal(n).divide(new BigDecimal(100)).setScale(1, BigDecimal.ROUND_HALF_UP), unit: "%")
    } else if (ep == 2 && cluster == 0x042A && attr == 0x0000) {
        sendEvent(name: "particulateMatter", value: n, unit: "µg/m³")
    } else if (ep == 3 && cluster == 0x040D && attr == 0x0000) {
        sendEvent(name: "carbonDioxide", value: n, unit: "ppm")
    } else if ((ep == 4 || ep == 5) && cluster == 0x000C && attr == 0x0055) {
        // Zigbee Analog Input PresentValue is IEEE-754 single precision.
        Float f = Float.intBitsToFloat((int)(Long.parseLong(raw, 16) & 0xFFFFFFFFL))
        if (ep == 4) sendEvent(name: "tvoc", value: f, unit: "ppb")
        else sendEvent(name: "aqi", value: f, unit: "UBA")
    }
}

