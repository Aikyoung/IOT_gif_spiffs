# 30.201 Wireless Communications & IOT (IOT T8 Project)

# GIF display with rainmaker control documentation

This repository contains information on how to display and control 2 GIFs on a JC2432W328C board equipped with the ST7789 display controller via a mounted SD card. It is part of a 2-phase project with kaine119 to create a teaching feedback system.

## 📋 Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Project Structure](#project-structure)


## ✨ Features

- **GIF_To_RAW Conversion**: Python script to generate RAW format files for display on the JC2432W328C
- **Display control using rainmaker**: Real-time control of displayed GIF using rainmaker app


## 🔧 Requirements

### Hardware

- JC2432W328C board equipped with the ST7789 display controller.
- FAT32 formatted SD card

### Software

- ESP-IDF V5.5.2 (or higher)


## 🚀 Quick Start

### 1. RAW file preparation

**Step 1: Use make_raw.py to convert GIF format files into RAW format**
Due to the JC2432W328C board not having a PSRAM, it is taxing on the RAM to simultaneously handle rainmaker and display memory requirements. Hence the GIF is formatted into RAW data directly to reduce the need for decoding.

**Step 2: Insert RAW files into SD card and mount the SD card**
Reduces flash storage required to store larger RAW files, and eases the process of changing the GIF to be displayed

**Step 3: Build and flash this project on the JC2432W328C board**
This existing project is designed to be able to be built and flash without prior adjustments. 
For user adjustment, 
-   GIF file names can be changed under main/main.c
-   Rainmaker functionality can be changed under main/main.c

**Step 4: Claim the ESP32 under the rainmaker app to control/create automation**
If not previously provisioned, the monitor produces a rainmaker link that can be used to generate a rainmaker qr code for provisioning. Afterwards, the gif display will be controlled by the rainmaker app.


## 📁 Project Structure

```
IOT_GIF_SPIFFS/
├── Finalised gifs/
│   ├── gif_a.raw                 # Confusion gif
│   ├── gif_b.raw                 # Big brain gif
│
├── main/
│   ├── CMakeLists.txt            # contains requirements list
│   ├── idf_component.yml         # Manifest file for component manager
│   ├── main.c                    # Main script
│   ├── tjpgd.h                   # Arduino TJpg_Decoder
│   └── tjpgdcnf.h                # TJpgDec System Configurations
│
├── managed_components/           # Components list for main/CMakeLists.txt
│
├── analyze_gifs.py               # Gif parameters for debugging
├── CMakeLists.txt                # Top level CMake file for project
├── dependencies.lock             # ESP-IDF dependencies
├── make_raw.py                   # Python script to convert GIF to raw
├── partitions.csv                # Partitioning of flash
├── README.md                     # This file
├── sdkconfig                     # Automatically generated config file
└── sdkconfig.default             # Default config
```
