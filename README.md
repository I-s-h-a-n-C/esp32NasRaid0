# esp32NasRaid0
An esp32 nas running RAID 0 on sd card modules<br>

I wanted to make a NAS for my house, but I wanted it to be as cheap as possible and work on an SD card, so that I can take it with me on the go.

## What it does
This is a Network Attached Storage system built on an ESP32 that uses two SD cards in RAID 0 configuration to double the storage speed and capacity. It has a web interface where you can upload, download, delete, and organize files, create folders, and search through everything.

## How I built it
Hardware
ESP32 Dev Board

2x SD card modules

2x Micro SD cards

Custom 3D printed satellite-themed case

![WhatsApp Image 2025-12-01 at 17 26 40_c2888e0f](https://github.com/user-attachments/assets/02a92f56-f3af-4968-90e4-b71ab4c163df)

## Software
The system runs on the ESP32 and creates its own WiFi network. When you connect to it, you get a web interface that lets you manage files. Here's what it does:

## RAID 0 Implementation
The tricky part was getting two SD cards to work together. They share the same SPI pins (SCK, MISO, MOSI) but use different CS pins (15 and 25). The ESP32 switches between them really fast to read/write files. Files get spread across both cards automatically.

![WhatsApp Image 2025-12-02 at 10 51 50_4d072edc](https://github.com/user-attachments/assets/bdcd0a9b-e209-4359-ba12-08735c467816)


## Web Interface Features
* Drag and drop file upload with progress bars
* Folder creation and navigation
* Search functionality for finding files
* Real-time storage status showing both SD cards
* WiFi signal strength display
* System memory usage monitoring

<img width="1918" height="861" alt="Screenshot 2025-12-02 104346" src="https://github.com/user-attachments/assets/5c34a166-3ed8-4d24-b1fd-f80924c29d1f" />
<img width="1919" height="865" alt="Screenshot 2025-12-02 104355" src="https://github.com/user-attachments/assets/6ca6666b-c1b4-4a05-a540-d3a5c8b9f161" />

## Hardware Connections
<img width="902" height="723" alt="image" src="https://github.com/user-attachments/assets/c815c46f-086f-4053-9f6d-7b9245312775" />
<img width="1003" height="477" alt="image" src="https://github.com/user-attachments/assets/8d89d526-f94a-4327-8f7d-74436e535a80" />


SD Card 1:<br>
CS  -> GPIO 15 <br>
SCK -> GPIO 18<br>
MISO-> GPIO 19<br>
MOSI-> GPIO 23<br>

SD Card 2:<br>
CS  -> GPIO 25<br>
SCK -> GPIO 18<br>
MISO-> GPIO 19<br>
MOSI-> GPIO 23<br>


## File Operations

* Upload files up to 10MB
* Download files with correct file types
* Delete files and folders
* Create new directories
* Refresh file list without page reload

## 3D Printed Case
The case files are in the 3dModels folder. It's designed to look like a satellite and protects all the components.

<img width="453" height="400" alt="Screenshot 2025-12-01 173535" src="https://github.com/user-attachments/assets/ba9f13f5-8967-4f97-b3cd-80709f38c1ca" />
<img width="1420" height="843" alt="image" src="https://github.com/user-attachments/assets/6501738c-c4f6-464f-925d-43810ede2fa8" />
<img width="1418" height="842" alt="image" src="https://github.com/user-attachments/assets/e789d7f3-ff35-42e6-97cc-c396cd9786fc" />
<img width="1421" height="948" alt="image" src="https://github.com/user-attachments/assets/3593df04-87ff-45a1-9445-d98ab7ac616a" />
<img width="1417" height="952" alt="image" src="https://github.com/user-attachments/assets/f3286e17-b0c3-4b48-ae28-ec1d1f1662ee" />





## Demo on YT
https://www.youtube.com/watch?v=oVuj8RxJoR4
