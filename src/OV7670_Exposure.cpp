#include "OV7670.h"

// 添加到OV7670類的實現中，用於控制曝光設置

// 設置手動曝光值 (0-255)
void OV7670::setManualExposure(uint8_t exposure) {
  // 禁用自動曝光控制
  uint8_t com8 = 0;
  i2c.writeRegister(ADDR, REG_COM8, com8 & ~COM8_AEC);
  
  // 設置曝光值
  i2c.writeRegister(ADDR, REG_AECH, (exposure >> 8) & 0x3F);
  i2c.writeRegister(ADDR, REG_COM1, exposure & 0xFF);
}

// 啟用自動曝光控制
void OV7670::enableAEC() {
  uint8_t com8 = 0;
  i2c.writeRegister(ADDR, REG_COM8, com8 | COM8_AEC);
}

// 設置曝光級別 (-2 到 +2)
// level: -2 (較暗), -1, 0 (正常), 1, 2 (較亮)
void OV7670::setExposureLevel(int level) {
  // 讀取當前的 COM8 值
  uint8_t com8 = 0xE7; // 默認值
  
  // 啟用自動曝光控制
  com8 |= COM8_AEC;
  i2c.writeRegister(ADDR, REG_COM8, com8);
  
  // 根據曝光級別調整 AEC 參數
  if (level < 0) {
    // 降低曝光 - 減少 AEC 最大值
    uint8_t aew = 0x40 + (level * 10); // 基準值減少
    uint8_t aeb = 0x30 + (level * 10); // 基準值減少
    i2c.writeRegister(ADDR, REG_AEW, aew);
    i2c.writeRegister(ADDR, REG_AEB, aeb);
  } else if (level > 0) {
    // 增加曝光 - 增加 AEC 最大值
    uint8_t aew = 0x40 + (level * 10); // 基準值增加
    uint8_t aeb = 0x30 + (level * 10); // 基準值增加
    i2c.writeRegister(ADDR, REG_AEW, aew);
    i2c.writeRegister(ADDR, REG_AEB, aeb);
  } else {
    // 正常曝光 - 使用默認值
    i2c.writeRegister(ADDR, REG_AEW, 0x40);
    i2c.writeRegister(ADDR, REG_AEB, 0x30);
  }
  
  // 調整自動曝光控制的速度
  if (level < 0) {
    // 較暗環境 - 較慢的AEC響應
    i2c.writeRegister(ADDR, REG_COM4, 0x40); // 較慢的AEC/AGC
  } else if (level > 0) {
    // 較亮環境 - 較快的AEC響應
    i2c.writeRegister(ADDR, REG_COM4, 0x80); // 較快的AEC/AGC
  } else {
    // 正常環境 - 中等AEC響應
    i2c.writeRegister(ADDR, REG_COM4, 0x00); // 默認AEC/AGC
  }
}

// 設置夜間模式
void OV7670::setNightMode(bool enable) {
  uint8_t com11 = 0;
  
  if (enable) {
    // 啟用夜間模式
    com11 |= COM11_NIGHT;
    // 設置幀率降低為自動，以增加曝光時間
    com11 |= COM11_NMFR;
  }
  
  i2c.writeRegister(ADDR, REG_COM11, com11);
}

// 設置亮度 (-2 到 +2)
void OV7670::setBrightness(int brightness) {
  // 亮度調整範圍從-2到+2
  uint8_t value = 0x80 + brightness * 0x10;
  i2c.writeRegister(ADDR, REG_BLUE, value); // 藍色通道偏移
  i2c.writeRegister(ADDR, REG_RED, value);  // 紅色通道偏移
}