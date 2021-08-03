#include <sched.h>
#include <sys/cdefs.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>

#include <libusb-1.0/libusb.h>

#include "cereal/gen/cpp/car.capnp.h"
#include "cereal/messaging/messaging.h"
#include "selfdrive/common/params.h"
#include "selfdrive/common/swaglog.h"
#include "selfdrive/common/timing.h"
#include "selfdrive/common/util.h"
#include "selfdrive/hardware/hw.h"
#include "selfdrive/locationd/ublox_msg.h"

#include "selfdrive/boardd/panda.h"
#include "selfdrive/boardd/pigeon.h"

#define MAX_IR_POWER 0.5f
#define MIN_IR_POWER 0.0f
#define CUTOFF_IL 200
#define SATURATE_IL 1600
#define NIBBLE_TO_HEX(n) ((n) < 10 ? (n) + '0' : ((n) - 10) + 'a')

Panda *main_panda = nullptr;
Panda *aux_panda = nullptr;
uint8_t main_shift = 0;
uint8_t aux_shift = 3;
std::vector<std::string> pandas_detected = { };
std::vector<std::string> pandas_connected = { };
std::atomic<bool> safety_setter_thread_running(false);
std::atomic<bool> ignition(false);

ExitHandler do_exit;


void safety_setter_thread() {
  LOGD("Starting safety setter thread");
  // diagnostic only is the default, needed for VIN query
  main_panda->set_safety_model(cereal::CarParams::SafetyModel::ELM327);
  if (aux_panda != nullptr) { aux_panda->set_safety_model(cereal::CarParams::SafetyModel::ELM327); }

  Params p = Params();

  // switch to SILENT when CarVin param is read
  while (true) {
    if (do_exit || !main_panda->connected) {
      safety_setter_thread_running = false;
      return;
    };

    std::string value_vin = p.get("CarVin");
    if (value_vin.size() > 0) {
      // sanity check VIN format
      assert(value_vin.size() == 17);
      LOGW("got CarVin %s", value_vin.c_str());
      break;
    }
    util::sleep_for(100);
  }

  // VIN query done, stop listening to OBDII
  main_panda->set_safety_model(cereal::CarParams::SafetyModel::ELM327, 1);
  if (aux_panda != nullptr) { aux_panda->set_safety_model(cereal::CarParams::SafetyModel::ELM327, 1); }

  std::string params;
  LOGW("waiting for params to set safety model");
  while (true) {
    if (do_exit || !main_panda->connected) {
      safety_setter_thread_running = false;
      return;
    };

    if (p.getBool("ControlsReady")) {
      params = p.get("CarParams");
      if (params.size() > 0) break;
    }
    util::sleep_for(100);
  }
  LOGW("got %d bytes CarParams", params.size());

  AlignedBuffer aligned_buf;
  capnp::FlatArrayMessageReader cmsg(aligned_buf.align(params.data(), params.size()));
  cereal::CarParams::Reader car_params = cmsg.getRoot<cereal::CarParams>();
  cereal::CarParams::SafetyModel safety_model = car_params.getSafetyModel();

  main_panda->set_unsafe_mode(0);  // see safety_declarations.h for allowed values

  auto safety_param = car_params.getSafetyParam();
  LOGW("setting safety model: %d with param %d", (int)safety_model, safety_param);

  main_panda->set_safety_model(safety_model, safety_param);
  if (aux_panda != nullptr) { aux_panda->set_safety_model(safety_model, safety_param); }

  safety_setter_thread_running = false;
}

bool usb_connect() {
  static bool connected_once = false;

  std::unique_ptr<Panda> tmp_panda;
  try {
    //assert(pandas_detected.size() > 0);
    assert(main_panda == nullptr || aux_panda == nullptr);
    tmp_panda = std::make_unique<Panda>(pandas_detected[0]);
    pandas_connected.push_back(pandas_detected[0]);
    pandas_detected.erase(std::find(pandas_detected.begin(), pandas_detected.end(), pandas_detected[0])); // Replace for cleaner method!
  } catch (std::exception &e) {
    return false;
  }

  Params params = Params();

  if (getenv("BOARDD_LOOPBACK")) {
    tmp_panda->set_loopback(true);
  }

  if (auto fw_sig = tmp_panda->get_firmware_version(); fw_sig) {
    params.put("PandaFirmware", (const char *)fw_sig->data(), fw_sig->size());

    // Convert to hex for offroad
    char fw_sig_hex_buf[16] = {0};
    const uint8_t *fw_sig_buf = fw_sig->data();
    for (size_t i = 0; i < 8; i++) {
      fw_sig_hex_buf[2*i] = NIBBLE_TO_HEX((uint8_t)fw_sig_buf[i] >> 4);
      fw_sig_hex_buf[2*i+1] = NIBBLE_TO_HEX((uint8_t)fw_sig_buf[i] & 0xF);
    }

    params.put("PandaFirmwareHex", fw_sig_hex_buf, 16);
    LOGW("fw signature: %.*s", 16, fw_sig_hex_buf);
  } else { return false; }

  // get panda serial
  if (auto serial = tmp_panda->get_serial(); serial) {
    params.put("PandaDongleId", serial->c_str(), serial->length());
    LOGW("panda serial: %s", serial->c_str());
  } else { return false; }

  // power on charging, only the first time. Panda can also change mode and it causes a brief disconneciton
#ifndef __x86_64__
  if (!connected_once) {
    tmp_panda->set_usb_power_mode(cereal::PandaState::UsbPowerMode::CDP);
  }
#endif

  if (tmp_panda->has_rtc) {
    setenv("TZ","UTC",1);
    struct tm sys_time = util::get_time();
    struct tm rtc_time = tmp_panda->get_rtc();

    if (!util::time_valid(sys_time) && util::time_valid(rtc_time)) {
      LOGE("System time wrong, setting from RTC. "
           "System: %d-%02d-%02d %02d:%02d:%02d RTC: %d-%02d-%02d %02d:%02d:%02d",
           sys_time.tm_year + 1900, sys_time.tm_mon + 1, sys_time.tm_mday,
           sys_time.tm_hour, sys_time.tm_min, sys_time.tm_sec,
           rtc_time.tm_year + 1900, rtc_time.tm_mon + 1, rtc_time.tm_mday,
           rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec);

      const struct timeval tv = {mktime(&rtc_time), 0};
      settimeofday(&tv, 0);
    }
  }

  connected_once = true;
  if (tmp_panda->hw_type != cereal::PandaState::PandaType::BLACK_PANDA && tmp_panda->hw_type != cereal::PandaState::PandaType::DOS) {// TESTING
    aux_panda = tmp_panda.release();
  } else {
    main_panda = tmp_panda.release();
  }
  return true;
}

// must be called before threads or with mutex
static bool usb_retry_connect() {
  
  LOGW("attempting to find boards");
  size_t detect_count = 0;
  while (detect_count == 0) {
    util::sleep_for(100);
    pandas_detected = Panda::list();
    detect_count = pandas_detected.size();
  }
  
  for(int i=0; i<detect_count; ++i) {
    LOGW("board #%d USB serial: %s", i, pandas_detected[i].c_str());
  }
  LOGW("total boards detected: %d", detect_count);

  while (!do_exit && (detect_count != pandas_connected.size()) ) {
    usb_connect();
    util::sleep_for(100);
  }
  if (main_panda) {
    LOGW("connected to main board: %s", main_panda->usb_serial.c_str());
  } else { return false; }
  if (aux_panda) {
    LOGW("connected to aux board: %s", aux_panda->usb_serial.c_str());
  }
  return !do_exit;
}

void can_recv(PubMaster &pm) {
  kj::Array<capnp::word> can_data;
  main_panda->can_receive(can_data, main_shift);
  auto bytes = can_data.asBytes();
  pm.send("can", bytes.begin(), bytes.size());
  
  if (aux_panda != nullptr) {
    kj::Array<capnp::word> can_data_aux;
    aux_panda->can_receive(can_data_aux, aux_shift);
    auto bytes_aux = can_data_aux.asBytes();
    pm.send("can", bytes_aux.begin(), bytes_aux.size());
    }
}

void can_send_thread(bool fake_send) {
  LOGD("start send thread");

  AlignedBuffer aligned_buf;
  Context * context = Context::create();
  SubSocket * subscriber = SubSocket::create(context, "sendcan");
  assert(subscriber != NULL);
  subscriber->setTimeout(100);

  // run as fast as messages come in
  while (!do_exit && main_panda->connected) {
    Message * msg = subscriber->receive();

    if (!msg) {
      if (errno == EINTR) {
        do_exit = true;
      }
      continue;
    }

    capnp::FlatArrayMessageReader cmsg(aligned_buf.align(msg));
    cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();

    //Dont send if older than 1 second
    if (nanos_since_boot() - event.getLogMonoTime() < 1e9) {
      if (!fake_send) {
        if (main_shift == 0) {
          main_panda->can_send(event.getSendcan());
        } else {
          aux_panda->can_send(event.getSendcan());
        }
        
      }
    }

    delete msg;
  }

  delete subscriber;
  delete context;
}

void can_recv_thread() {
  LOGD("start recv thread");

  // can = 8006
  PubMaster pm({"can"});

  // run at 100hz
  const uint64_t dt = 10000000ULL;
  uint64_t next_frame_time = nanos_since_boot() + dt;

  while (!do_exit && main_panda->connected) {
    can_recv(pm);

    uint64_t cur_time = nanos_since_boot();
    int64_t remaining = next_frame_time - cur_time;
    if (remaining > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
    } else {
      if (ignition) {
        LOGW("missed cycles (%d) %lld", (int)-1*remaining/dt, remaining);
      }
      next_frame_time = cur_time;
    }

    next_frame_time += dt;
  }
}

void panda_state_thread(bool spoofing_started) {
  LOGD("start panda state thread");
  PubMaster pm({"pandaState"});

  uint32_t no_ignition_cnt = 0;
  bool ignition_last = false;
  Params params = Params();

  // Broadcast empty pandaState message when panda is not yet connected
  while (!do_exit && !main_panda) {
    MessageBuilder msg;
    auto pandaState  = msg.initEvent().initPandaState();

    pandaState.setPandaType(cereal::PandaState::PandaType::UNKNOWN);
    pm.send("pandaState", msg);
    util::sleep_for(500);
  }

  // run at 2hz
  while (!do_exit && main_panda->connected) {
    health_t pandaState = main_panda->get_state();

    if (spoofing_started) {
      pandaState.ignition_line = 1;
    }

    // Make sure CAN buses are live: safety_setter_thread does not work if Panda CAN are silent and there is only one other CAN node
    if (pandaState.safety_model == (uint8_t)(cereal::CarParams::SafetyModel::SILENT)) {
      main_panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
    }

    if (aux_panda != nullptr) {
      health_t pandaState_aux = aux_panda->get_state();

      if (pandaState_aux.safety_model == (uint8_t)(cereal::CarParams::SafetyModel::SILENT)) {
        aux_panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
      }
      if (!ignition && (pandaState.safety_model != (uint8_t)(cereal::CarParams::SafetyModel::NO_OUTPUT))) {
      aux_panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
      }

      if (main_shift == 0) {
        ignition = ((pandaState.ignition_line != 0) || (pandaState.ignition_can != 0));
      } else {
        ignition = ((pandaState_aux.ignition_line != 0) || (pandaState_aux.ignition_can != 0));
      }
    }

    if (ignition) {
      no_ignition_cnt = 0;
    } else {
      no_ignition_cnt += 1;
    }

//#ifndef __x86_64__
    bool power_save_desired = !ignition;
    if (pandaState.power_save_enabled != power_save_desired) {
      main_panda->set_power_saving(power_save_desired);
      if (aux_panda != nullptr) { aux_panda->set_power_saving(power_save_desired); }
    }

    // set safety mode to NO_OUTPUT when car is off. ELM327 is an alternative if we want to leverage athenad/connect
    if (!ignition && (pandaState.safety_model != (uint8_t)(cereal::CarParams::SafetyModel::NO_OUTPUT))) {
      main_panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
    }
//#endif

    // clear VIN, CarParams, and set new safety on car start
    if (ignition && !ignition_last) {
      params.clearAll(CLEAR_ON_IGNITION_ON);

      if (!safety_setter_thread_running) {
        safety_setter_thread_running = true;
        std::thread(safety_setter_thread).detach();
      } else {
        LOGW("Safety setter thread already running");
      }
    } else if (!ignition && ignition_last) {
      params.clearAll(CLEAR_ON_IGNITION_OFF);
    }

    // Write to rtc once per minute when no ignition present
    if ((main_panda->has_rtc) && !ignition && (no_ignition_cnt % 120 == 1)) {
      // Write time to RTC if it looks reasonable
      setenv("TZ","UTC",1);
      struct tm sys_time = util::get_time();

      if (util::time_valid(sys_time)) {
        struct tm rtc_time = main_panda->get_rtc();
        double seconds = difftime(mktime(&rtc_time), mktime(&sys_time));

        if (std::abs(seconds) > 1.1) {
          main_panda->set_rtc(sys_time);
          LOGW("Updating panda RTC. dt = %.2f "
               "System: %d-%02d-%02d %02d:%02d:%02d RTC: %d-%02d-%02d %02d:%02d:%02d",
               seconds,
               sys_time.tm_year + 1900, sys_time.tm_mon + 1, sys_time.tm_mday,
               sys_time.tm_hour, sys_time.tm_min, sys_time.tm_sec,
               rtc_time.tm_year + 1900, rtc_time.tm_mon + 1, rtc_time.tm_mday,
               rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec);
        }
      }
    }

    ignition_last = ignition;
    uint16_t fan_speed_rpm = main_panda->get_fan_speed();

    // build msg
    MessageBuilder msg;
    auto evt = msg.initEvent();
    evt.setValid(main_panda->comms_healthy);

    auto ps = evt.initPandaState();
    ps.setUptime(pandaState.uptime);

    if (Hardware::TICI()) {
      double read_time = millis_since_boot();
      ps.setVoltage(std::stoi(util::read_file("/sys/class/hwmon/hwmon1/in1_input")));
      ps.setCurrent(std::stoi(util::read_file("/sys/class/hwmon/hwmon1/curr1_input")));
      read_time = millis_since_boot() - read_time;
      if (read_time > 50) {
        LOGW("reading hwmon took %lfms", read_time);
      }
    } else {
      ps.setVoltage(pandaState.voltage);
      ps.setCurrent(pandaState.current);
    }
    
    ps.setIgnitionLine(ignition);
    ps.setIgnitionCan(pandaState.ignition_can);
    ps.setControlsAllowed(pandaState.controls_allowed);
    ps.setGasInterceptorDetected(pandaState.gas_interceptor_detected);
    ps.setHasGps(true);
    ps.setCanRxErrs(pandaState.can_rx_errs);
    ps.setCanSendErrs(pandaState.can_send_errs);
    ps.setCanFwdErrs(pandaState.can_fwd_errs);
    ps.setGmlanSendErrs(pandaState.gmlan_send_errs);
    ps.setPandaType(main_panda->hw_type);
    ps.setUsbPowerMode(cereal::PandaState::UsbPowerMode(pandaState.usb_power_mode));
    ps.setSafetyModel(cereal::CarParams::SafetyModel(pandaState.safety_model));
    ps.setSafetyParam(pandaState.safety_param);
    ps.setFanSpeedRpm(fan_speed_rpm);
    ps.setFaultStatus(cereal::PandaState::FaultStatus(pandaState.fault_status));
    ps.setPowerSaveEnabled((bool)(pandaState.power_save_enabled));
    ps.setHeartbeatLost((bool)(pandaState.heartbeat_lost));
    ps.setHarnessStatus(cereal::PandaState::HarnessStatus(pandaState.car_harness_status));

    // Convert faults bitset to capnp list
    std::bitset<sizeof(pandaState.faults) * 8> fault_bits(pandaState.faults);
    auto faults = ps.initFaults(fault_bits.count());

    size_t i = 0;
    for (size_t f = size_t(cereal::PandaState::FaultType::RELAY_MALFUNCTION);
        f <= size_t(cereal::PandaState::FaultType::INTERRUPT_RATE_TICK); f++) {
      if (fault_bits.test(f)) {
        faults.set(i, cereal::PandaState::FaultType(f));
        i++;
      }
    }
    pm.send("pandaState", msg);
    main_panda->send_heartbeat();
    if (aux_panda != nullptr) { aux_panda->send_heartbeat(); }
    util::sleep_for(500);
  }
}

void hardware_control_thread() {
  LOGD("start hardware control thread");
  SubMaster sm({"deviceState", "driverCameraState"});

  uint64_t last_front_frame_t = 0;
  uint16_t prev_fan_speed = 999;
  uint16_t ir_pwr = 0;
  uint16_t prev_ir_pwr = 999;
  bool prev_charging_disabled = false;
  unsigned int cnt = 0;

  FirstOrderFilter integ_lines_filter(0, 30.0, 0.05);

  while (!do_exit && main_panda->connected) {
    cnt++;
    sm.update(1000); // TODO: what happens if EINTR is sent while in sm.update?

    if (!Hardware::PC() && sm.updated("deviceState")) {
      // Charging mode
      bool charging_disabled = sm["deviceState"].getDeviceState().getChargingDisabled();
      if (charging_disabled != prev_charging_disabled) {
        if (charging_disabled) {
          main_panda->set_usb_power_mode(cereal::PandaState::UsbPowerMode::CLIENT);
          LOGW("TURN OFF CHARGING!\n");
        } else {
          main_panda->set_usb_power_mode(cereal::PandaState::UsbPowerMode::CDP);
          LOGW("TURN ON CHARGING!\n");
        }
        prev_charging_disabled = charging_disabled;
      }
    }

    // Other pandas don't have fan/IR to control
    if (main_panda->hw_type != cereal::PandaState::PandaType::UNO && main_panda->hw_type != cereal::PandaState::PandaType::DOS) continue;
    if (sm.updated("deviceState")) {
      // Fan speed
      uint16_t fan_speed = sm["deviceState"].getDeviceState().getFanSpeedPercentDesired();
      if (fan_speed != prev_fan_speed || cnt % 100 == 0) {
        main_panda->set_fan_speed(fan_speed);
        prev_fan_speed = fan_speed;
      }
    }
    if (sm.updated("driverCameraState")) {
      auto event = sm["driverCameraState"];
      int cur_integ_lines = event.getDriverCameraState().getIntegLines();

      if (Hardware::TICI()) {
        cur_integ_lines = integ_lines_filter.update(cur_integ_lines);
      }
      last_front_frame_t = event.getLogMonoTime();

      if (cur_integ_lines <= CUTOFF_IL) {
        ir_pwr = 100.0 * MIN_IR_POWER;
      } else if (cur_integ_lines > SATURATE_IL) {
        ir_pwr = 100.0 * MAX_IR_POWER;
      } else {
        ir_pwr = 100.0 * (MIN_IR_POWER + ((cur_integ_lines - CUTOFF_IL) * (MAX_IR_POWER - MIN_IR_POWER) / (SATURATE_IL - CUTOFF_IL)));
      }
    }
    // Disable ir_pwr on front frame timeout
    uint64_t cur_t = nanos_since_boot();
    if (cur_t - last_front_frame_t > 1e9) {
      ir_pwr = 0;
    }

    if (ir_pwr != prev_ir_pwr || cnt % 100 == 0 || ir_pwr >= 50.0) {
      main_panda->set_ir_pwr(ir_pwr);
      prev_ir_pwr = ir_pwr;
    }

  }
}

static void pigeon_publish_raw(PubMaster &pm, const std::string &dat) {
  // create message
  MessageBuilder msg;
  msg.initEvent().setUbloxRaw(capnp::Data::Reader((uint8_t*)dat.data(), dat.length()));
  pm.send("ubloxRaw", msg);
}

void pigeon_thread() {
  PubMaster pm({"ubloxRaw"});
  bool ignition_last = false;

  Pigeon *pigeon = Hardware::TICI() ? Pigeon::connect("/dev/ttyHS0") : Pigeon::connect(main_panda);

  std::unordered_map<char, uint64_t> last_recv_time;
  std::unordered_map<char, int64_t> cls_max_dt = {
    {(char)ublox::CLASS_NAV, int64_t(900000000ULL)}, // 0.9s
    {(char)ublox::CLASS_RXM, int64_t(900000000ULL)}, // 0.9s
  };

  while (!do_exit && main_panda->connected) {
    bool need_reset = false;
    std::string recv = pigeon->receive();

    // Parse message header
    if (ignition && recv.length() >= 3) {
      if (recv[0] == (char)ublox::PREAMBLE1 && recv[1] == (char)ublox::PREAMBLE2) {
        const char msg_cls = recv[2];
        uint64_t t = nanos_since_boot();
        if (t > last_recv_time[msg_cls]) {
          last_recv_time[msg_cls] = t;
        }
      }
    }

    // Check based on message frequency
    for (const auto& [msg_cls, max_dt] : cls_max_dt) {
      int64_t dt = (int64_t)nanos_since_boot() - (int64_t)last_recv_time[msg_cls];
      if (ignition_last && ignition && dt > max_dt) {
        LOG("ublox receive timeout, msg class: 0x%02x, dt %llu", msg_cls, dt);
        // TODO: turn on reset after verification of logs
        // need_reset = true;
      }
    }

    // Check based on null bytes
    if (ignition && recv.length() > 0 && recv[0] == (char)0x00) {
      need_reset = true;
      LOGW("received invalid ublox message while onroad, resetting panda GPS");
    }

    if (recv.length() > 0) {
      pigeon_publish_raw(pm, recv);
    }

    // init pigeon on rising ignition edge
    // since it was turned off in low power mode
    if((ignition && !ignition_last) || need_reset) {
      pigeon->init();

      // Set receive times to current time
      uint64_t t = nanos_since_boot() + 10000000000ULL; // Give ublox 10 seconds to start
      for (const auto& [msg_cls, dt] : cls_max_dt) {
        last_recv_time[msg_cls] = t;
      }
    } else if (!ignition && ignition_last) {
      // power off on falling edge of ignition
      LOGD("powering off pigeon\n");
      pigeon->stop();
      pigeon->set_power(false);
    }

    ignition_last = ignition;

    // 10ms - 100 Hz
    util::sleep_for(10);
  }

  delete pigeon;
}


int main() {
  int err;
  LOGW("starting boardd");

  // set process priority and affinity
  err = set_realtime_priority(54);
  LOG("set priority returns %d", err);

  err = set_core_affinity(Hardware::TICI() ? 4 : 3);
  LOG("set affinity returns %d", err);

  if (getenv("AUX_CAN_DRIVE") != nullptr) { main_shift = 3; aux_shift = 0; }

  while (!do_exit) {
    std::vector<std::thread> threads;
    threads.push_back(std::thread(panda_state_thread, getenv("STARTED") != nullptr));

    // connect to the board
    if (usb_retry_connect()) {
      threads.push_back(std::thread(can_send_thread, getenv("FAKESEND") != nullptr));
      threads.push_back(std::thread(can_recv_thread));
      threads.push_back(std::thread(hardware_control_thread));
      threads.push_back(std::thread(pigeon_thread));
    }

    for (auto &t : threads) t.join();

    delete main_panda;
    delete aux_panda;
    main_panda = nullptr;
    aux_panda = nullptr;
    pandas_connected.clear();
  }
}
