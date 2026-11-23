/**
 * Hubitat driver for the ESP32-C6 Air Quality Monitor.
 * Temperature/humidity use endpoint 1; PM2.5, eCO2, TVOC, and AQI use endpoints 2-5.
 */
metadata {
    definition(name: "ESP32-C6 Air Quality Monitor", namespace: "local", author: "Local") {
        capability "Sensor"
        capability "TemperatureMeasurement"
        capability "RelativeHumidityMeasurement"
        capability "CarbonDioxideMeasurement"
        capability "Configuration"
        capability "Refresh"
        attribute "particulateMatter", "number"
        attribute "tvoc", "number"
        attribute "aqi", "number"
        attribute "zigbeeStatus", "string"
    }
    preferences {
        section("Device reporting and display") {
            input name: "reportInterval", type: "enum", title: "Reporting interval", options: ["10":"10 seconds", "30":"30 seconds", "60":"1 minute", "300":"5 minutes", "900":"15 minutes"], defaultValue: "60", required: true
            input name: "temperatureUnit", type: "enum", title: "Temperature display", options: ["C":"Celsius", "F":"Fahrenheit"], defaultValue: "C", required: true
        }
        section("Screen alert thresholds (leave blank to disable)") {
            input name: "alertTemperature", type: "decimal", title: "Temperature threshold (selected display unit)", required: false
            input name: "alertHumidity", type: "decimal", title: "Humidity threshold (%)", required: false
            input name: "alertPm25", type: "decimal", title: "PM2.5 threshold (ug/m3)", required: false
            input name: "alertEco2", type: "number", title: "eCO2 threshold (ppm)", required: false
            input name: "alertTvoc", type: "number", title: "TVOC threshold (ppb)", required: false
            input name: "alertAqi", type: "decimal", title: "AQI threshold (out of 5)", required: false
        }
    }
}

def installed() { configure() }
def updated() { configure() }

def configure() {
    Integer interval = (settings.reportInterval ?: "60") as Integer
    Integer unit = settings.temperatureUnit == "F" ? 1 : 0
    List cmds = []
    // Mirror Hubitat preferences onto the ESP32 so its local screen and console
    // use the same temperature unit and refresh interval.
    cmds += zigbee.writeAttribute(0x0000, 0xF000, 0x21, interval, [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF001, 0x20, unit, [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF002, 0x29,
        scaledThreshold("alertTemperature", 10, -32768), [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF003, 0x21,
        scaledThreshold("alertHumidity", 10, 0xFFFF), [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF004, 0x21,
        scaledThreshold("alertPm25", 10, 0xFFFF), [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF005, 0x23,
        scaledThreshold("alertEco2", 1, 0x7FFFFFFF), [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF006, 0x23,
        scaledThreshold("alertTvoc", 1, 0x7FFFFFFF), [destEndpoint: 1])
    cmds += zigbee.writeAttribute(0x0000, 0xF007, 0x21,
        scaledThreshold("alertAqi", 10, 0xFFFF), [destEndpoint: 1])
    // The Hubitat preference sets the reporting cadence for every measurement.
    cmds += zigbee.configureReporting(0x0402, 0x0000, 0x29, interval, interval, 20, [destEndpoint: 1])
    cmds += zigbee.configureReporting(0x0405, 0x0000, 0x21, interval, interval, 100, [destEndpoint: 1])
    // Hubitat's configureReporting helper accepts an Integer delta. For Zigbee
    // single-precision float attributes, build the Configure Reporting frame
    // directly so the delta is encoded as the required IEEE-754 little-endian bytes.
    // Use a zero change threshold for floats so small sensor changes are still
    // reported on the selected interval instead of being hidden by a large delta.
    cmds << floatReportingCommand(0x042A, 0x0000, 2, interval, 0.0f)
    cmds << floatReportingCommand(0x040D, 0x0000, 3, interval, 0.0f)
    cmds << floatReportingCommand(0x000C, 0x0055, 4, interval, 0.0f)
    cmds << floatReportingCommand(0x000C, 0x0055, 5, interval, 0.0f)
    cmds += zigbee.readAttribute(0x0402, 0x0000, [destEndpoint: 1])
    cmds += zigbee.readAttribute(0x0405, 0x0000, [destEndpoint: 1])
    cmds += zigbee.readAttribute(0x042A, 0x0000, [destEndpoint: 2])
    cmds += zigbee.readAttribute(0x040D, 0x0000, [destEndpoint: 3])
    cmds += zigbee.readAttribute(0x000C, 0x0055, [destEndpoint: 4])
    cmds += zigbee.readAttribute(0x000C, 0x0055, [destEndpoint: 5])
    cmds += zigbee.readAttribute(0x0000, 0xF000, [destEndpoint: 1])
    cmds += zigbee.readAttribute(0x0000, 0xF001, [destEndpoint: 1])
    log.info "Configuring Zigbee reporting every ${interval}s; sending ${cmds.size()} configure/read commands"
    delayBetween(cmds, 200)
}

private Integer scaledThreshold(String settingName, Integer scale, Integer disabledValue) {
    Object raw = settings[settingName]
    if (raw == null || raw.toString().trim().isEmpty()) return disabledValue
    try {
        BigDecimal value = new BigDecimal(raw.toString().trim())
        (value * scale).setScale(0, BigDecimal.ROUND_HALF_UP).intValue()
    } catch (Exception ex) {
        log.warn "Ignoring invalid screen threshold for ${settingName}: ${raw}"
        disabledValue
    }
}

private String floatReportingCommand(Integer cluster, Integer attribute, Integer endpoint, Integer interval, Float delta) {
    String ieee754 = String.format("%08X", Float.floatToIntBits(delta))
    String littleEndian = ieee754.substring(6, 8) + ieee754.substring(4, 6) + ieee754.substring(2, 4) + ieee754.substring(0, 2)
    String epHex = String.format("%02X", endpoint)
    String clusterHex = String.format("%04X", cluster)
    String attrHex = String.format("%04X", attribute)
    String intervalHex = String.format("%04X", interval)
    "he cr 0x${device.deviceNetworkId} 0x${epHex} 0x${clusterHex} 0x${attrHex} 0x39 0x${intervalHex} 0x${intervalHex} {${littleEndian}}"
}

def refresh() { configure() }

// Hubitat's description parser exposes ZCL values as a normal hexadecimal number.
private String normalizedHex(String raw) {
    String hex = raw?.replaceFirst("(?i)^0x", "")
    if (!hex || (hex.length() % 2) != 0) return null
    hex
}

private BigInteger unsignedValue(String raw) {
    String hex = normalizedHex(raw)
    hex ? new BigInteger(hex, 16) : null
}

private Float floatValue(String raw) {
    String hex = normalizedHex(raw)
    if (!hex || hex.length() != 8) return null
    Float.intBitsToFloat((int)(Long.parseLong(hex, 16) & 0xFFFFFFFFL))
}

def parse(String description) {
    Map desc = zigbee.parseDescriptionAsMap(description)
    log.info "Received Zigbee frame: ${description}"
    if (!desc) return
    Integer ep = desc.endpoint ? Integer.parseInt(desc.endpoint, 16) : 1
    Integer cluster = desc.clusterInt ?: (desc.cluster ? Integer.parseInt(desc.cluster, 16) : 0)
    Integer attr = desc.attrInt ?: (desc.attrId ? Integer.parseInt(desc.attrId, 16) : -1)
    String raw = desc.value
    if (raw == null) {
        if (desc.command == "04") {
            log.info "ESP32 acknowledged a Hubitat attribute write"
            return
        }
        log.warn "Zigbee frame has no attribute value: ${description}"
        return
    }

    if (ep == 1 && cluster == 0x0000 && attr >= 0xF002 && attr <= 0xF007) {
        log.info "ESP32 acknowledged screen alert threshold attribute 0x${Integer.toHexString(attr)}"
    } else if (ep == 1 && cluster == 0x0000 && attr == 0xF000) {
        log.info "ESP32 acknowledged display/report interval setting: ${unsignedValue(raw)} seconds"
    } else if (ep == 1 && cluster == 0x0000 && attr == 0xF001) {
        log.info "ESP32 acknowledged display temperature unit setting: ${unsignedValue(raw) == 1 ? 'F' : 'C'}"
    } else if (ep == 1 && cluster == 0x0402 && attr == 0x0000) {
        BigInteger n = unsignedValue(raw)
        if (n == null) return
        if (n >= 0x8000) n -= 0x10000
        BigDecimal c = new BigDecimal(n).divide(new BigDecimal(100))
        BigDecimal shown = settings.temperatureUnit == "F" ? c * 9 / 5 + 32 : c
        sendEvent(name: "temperature", value: shown.setScale(1, BigDecimal.ROUND_HALF_UP), unit: settings.temperatureUnit ?: "C")
    } else if (ep == 1 && cluster == 0x0405 && attr == 0x0000) {
        BigInteger n = unsignedValue(raw)
        if (n != null) sendEvent(name: "humidity", value: new BigDecimal(n).divide(new BigDecimal(100)).setScale(1, BigDecimal.ROUND_HALF_UP), unit: "%")
    } else if (ep == 2 && cluster == 0x042A && attr == 0x0000) {
        Float pm25 = floatValue(raw)
        if (pm25 != null && !pm25.isNaN() && !pm25.isInfinite())
            sendEvent(name: "particulateMatter", value: pm25, unit: "ug/m3")
    } else if (ep == 3 && cluster == 0x040D && attr == 0x0000) {
        Float fraction = floatValue(raw)
        if (fraction != null && !fraction.isNaN() && !fraction.isInfinite())
            sendEvent(name: "carbonDioxide", value: Math.round(fraction * 1000000.0f), unit: "ppm")
    } else if ((ep == 4 || ep == 5) && cluster == 0x000C && attr == 0x0055) {
        Float value = floatValue(raw)
        if (value != null && !value.isNaN() && !value.isInfinite()) {
            if (ep == 4) sendEvent(name: "tvoc", value: value, unit: "ppb")
            else sendEvent(name: "aqi", value: value, unit: "out of 5")
        }
    } else {
        log.warn "Unmapped Zigbee attribute: endpoint=${ep}, cluster=0x${Integer.toHexString(cluster)}, attribute=0x${Integer.toHexString(attr)}, value=${raw}"
    }
}
