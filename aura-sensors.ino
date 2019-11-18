#include <HardwareSerial.h>

#include "src/UBLOX8/UBLOX8.h"
#include "src/serial_link/serial_link.h"

#include "setup_board.h"
#include "setup_sbus.h"
#include "setup_pwm.h"
#include "aura3_messages.h"

// master config (for messages and saving in eeprom)
message::config_master_t config_master;
message::config_imu_t config_imu;
message::config_actuators_t config_actuators;
message::config_airdata_t config_airdata;
message::config_power_t config_power;
message::config_led_t config_led;
int config_size = 0;

// IMU
int gyros_calibrated = 0; // 0 = uncalibrated, 1 = calibration in progress, 2 = calibration finished
float imu_calib[10]; // the calibrated version of the imu sensors
int16_t imu_packed[10]; // calibrated and packed version of the imu sensors
unsigned long imu_micros = 0;

// Controls and Actuators
float receiver_norm[SBUS_CHANNELS];
uint8_t receiver_flags = 0x00;
float autopilot_norm[SBUS_CHANNELS];
float actuator_norm[SBUS_CHANNELS];
uint16_t actuator_pwm[PWM_CHANNELS];
uint8_t test_pwm_channel = -1;

// GPS
UBLOX8 gps(&Serial3); // ublox m8n
bool new_gps_data = false;
ublox8_nav_pvt_t gps_data;

// Air Data
int airdata_error_count = 0;
float airdata_diffPress_pa = 0.0;
float airdata_temp_C = 0.0;

// Power
const float analogResolution = 65535.0f;
const float pwr_scale = 11.0f;
const float avionics_scale = 2.0f;
uint8_t avionics_pin;
uint8_t source_volt_pin;
uint8_t atto_volts_pin = A2;
uint8_t atto_amps_pin = A3;
float pwr1_v = 0.0;
float pwr2_v = 0.0;
float avionics_v = 0.0;
float pwr_a = 0.0;
    
// COMS
// Serial = usb, Serial1 connects to /dev/ttyO4 on beaglebone in
// aura-v2 and marmot-v1 hardware
unsigned long output_counter = 0;
SerialLink serial;

// force/hard-code a specific board config if desired
void force_config_aura_v2() {
    Serial.println("Forcing an aura v2 eeprom config");
    config_master.board = 1;    // 0 = marmot v1, 1 = aura v2
    config_imu.interface = 1;   // i2c
    config_imu.pin_or_address = 0x68; // mpu9250 i2c addr
    config_airdata.barometer = 1; // 1 = bmp280/i2c
    config_airdata.pitot = 0;     // 0 MS4525
    config_led.pin = 13;
    config_power.have_attopilot = true;
    config_actuators.act_gain[0] = 1.0;
    config_actuators.act_gain[1] = 1.0;
    config_actuators.act_gain[2] = -1.0;
    config_actuators.act_gain[3] = 1.0;
    config_actuators.act_gain[4] = -1.0;
    config_actuators.mix_vtail = true;
    config_actuators.mix_Gve = 1.0;
    config_actuators.mix_Gvr = 1.0;
    config_actuators.mix_flaperon = true;
    config_actuators.mix_Gfa = 1.0;
    config_actuators.mix_Gff = 1.0;
    config_actuators.mix_autocoord = true;
    config_actuators.mix_Gac = 0.25;
    config_actuators.sas_rollaxis = true;
    config_actuators.sas_pitchaxis = true;
    config_actuators.sas_yawaxis = true;
    config_actuators.sas_rollgain = 0.2;
    config_actuators.sas_pitchgain = 0.2;
    config_actuators.sas_yawgain = 0.2;
}

// force/hard-code a specific board config if desired
void force_config_talon_marmot() {
    Serial.println("Forcing a bfs/marmot eeprom config");
    config_master.board = 0;    // 0 = marmot v1, 1 = aura v2
    config_imu.interface = 0;   // spi
    config_imu.pin_or_address = 24; // marmot imu spi cs line
    config_airdata.barometer = 2; // 2 = swift
    config_airdata.pitot = 2;     // 2 = swift, 0x25
    config_airdata.swift_baro_addr = 0x24; // Idun = 0x24
    config_airdata.swift_pitot_addr = 0x25; // Idun = 0x24
    config_led.pin = 0;
    config_actuators.act_gain[0] = 1.0;
    config_actuators.act_gain[1] = 1.0;
    config_actuators.act_gain[2] = -1.0;
    config_actuators.act_gain[3] = 1.0;
    config_actuators.act_gain[4] = -1.0;
    config_actuators.mix_vtail = true;
    config_actuators.mix_Gve = 1.0;
    config_actuators.mix_Gvr = 1.0;
    config_actuators.mix_flaperon = true;
    config_actuators.mix_Gfa = 1.0;
    config_actuators.mix_Gff = 1.0;
    config_actuators.mix_autocoord = true;
    config_actuators.mix_Gac = 0.25;
    config_actuators.sas_rollaxis = true;
    config_actuators.sas_pitchaxis = true;
    config_actuators.sas_yawaxis = true;
    config_actuators.sas_rollgain = 0.2;
    config_actuators.sas_pitchgain = 0.2;
    config_actuators.sas_yawgain = 0.2;
}

void setup() {
    // put your setup code here, to run once:

    Serial.begin(DEFAULT_BAUD);
    delay(1000);  // hopefully long enough for serial to come alive

    serial.open(DEFAULT_BAUD, &Serial1);

    Serial.print("\nAura Sensors: Rev "); Serial.println(FIRMWARE_REV);
    Serial.println("You are seeing this message on the usb interface.");
    Serial.print("Sensor/config communication is on Serial1 @ ");
    Serial.print(DEFAULT_BAUD);
    Serial.println(" baud (N81) no flow control.");
    
    // The following code (when enabled) will force setting a specific
    // device serial number when the device boots:
    // set_serial_number(116);
    
    read_serial_number();
    
    if ( !config_read_eeprom() ) {
        Serial.println("Resetting eeprom to default values.");
        config_load_defaults();
        config_write_eeprom();
    } else {
        Serial.println("Successfully loaded eeprom config.");
    }
    
    Serial.print("Serial Number: ");
    Serial.println(read_serial_number());
    delay(100);

    // force/hard-code a specific board config if desired
    // force_config_aura_v2();
    // force_config_talon_marmot();
    
    // initialize the IMU
    imu_setup();
    delay(100);

    // initialize the SBUS receiver
    sbus_setup();

    // initialize PWM output
    pwm_setup();

    // initialize the gps receiver
    gps.begin(115200);

    // initialize air data (marmot v1)
    // config_airdata.barometer = 3; // 3 = bmp180
    airdata_setup();
    
    // set up ADC0
    analogReadResolution(16);

    // power sensing
    if ( config_master.board == 0 ) {
        // Marmot v1
        #ifdef HAVE_TEENSY36    // A22 doesn't exist for teensy3.2
        avionics_pin = A22;
        #endif
        source_volt_pin = 15;
    } else if ( config_master.board == 1 ) {
        // Aura v2
        avionics_pin = A1;
        source_volt_pin = A0;
        if ( config_power.have_attopilot ) {
            Serial.println("Attopilot enabled.");
            atto_volts_pin = A2;
            atto_amps_pin = A3;
        }
    } else {
        Serial.println("Master board configuration not defined correctly.");
    }
    
    // led for status blinking if defined
    led_setup();

    Serial.println("Ready and transmitting...");
}

void loop() {
    // put your main code here, to run repeatedly:
    static elapsedMillis mainTimer = 0;
    static elapsedMillis debugTimer = 0;
       
    // When new IMU data is ready (new pulse from IMU), go out and grab the IMU data
    // and output fresh IMU message plus the most recent data from everything else.
    if ( mainTimer >= DT_MILLIS ) {
        mainTimer -= DT_MILLIS;
        
        // top priority, used for timing sync downstream.
        imu_update();
        
        // output keyed off new IMU data
        output_counter += write_pilot_in_bin();
        output_counter += write_gps_bin();
        output_counter += write_airdata_bin();
        output_counter += write_power_bin();
        // do a little extra dance with the return value because write_status_info_bin()
        // can reset output_counter (but that gets ignored if we do the math in one step)
        uint8_t result = write_status_info_bin();
        output_counter += result;
        output_counter += write_imu_bin(); // write IMU data last as an implicit 'end of data frame' marker.

        // 10hz human debugging output, but only after gyros finish calibrating
        if ( debugTimer >= 100 && gyros_calibrated == 2) {
            debugTimer = 0;
            // write_pilot_in_ascii();
            // write_actuator_out_ascii();
            // write_gps_ascii();
            // write_airdata_ascii();
            // write_status_info_ascii();
            // write_imu_ascii();
        }

        // uncomment this next line to test drive individual servo channels
        // (for debugging or validation.)
        test_pwm_channel = -1;  // zero is throttle so be careful!
        if ( test_pwm_channel >= 0 ) {
            pwm_update();
        }

        // poll the pressure sensors
        airdata_update();

        // battery voltage
        uint16_t ain;
        ain = analogRead(source_volt_pin);
        pwr1_v = ((float)ain) * 3.3 / analogResolution * pwr_scale;

        ain = analogRead(avionics_pin);
        avionics_v = ((float)ain) * 3.3 / analogResolution * avionics_scale;

        if ( config_power.have_attopilot ) {
            ain = analogRead(atto_volts_pin);
            // Serial.print("atto volts: ");
            // Serial.println( ((float)ain) * 3.3 / analogResolution );
        }
    }

    // suck in any available gps bytes
    if ( gps.read_ublox8() ) {
        new_gps_data = true;
        // gps_data = gps.get_data();
        gps.update_data(&gps_data, sizeof(gps_data));
    }

    // keep processing while there is data in the uart buffer
    while ( sbus_process() );

    // suck in any host commmands (flight control updates, etc.)
    // debug: while ( Serial1.available() ) Serial1.read();
    while ( serial.update() ) {
        parse_message_bin( serial.pkt_id, serial.payload, serial.pkt_len );
    }

    // blink the led on boards that support it
    led_update();
}
