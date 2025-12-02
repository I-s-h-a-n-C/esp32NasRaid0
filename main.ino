#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "esp_task_wdt.h"
#include <vector>
#include <algorithm>
#include <mutex>

const char* ssid = "KakaliIshan Abode";
const char* password = "PurpleCow2013#";

const char* nas_username = "admin";
const char* nas_password = "123";

// SD Card pins (RAID 0 configuration)
#define SD1_CS     15
#define SD2_CS     25
#define SD_SCK     18
#define SD_MISO    19
#define SD_MOSI    23

#define WDT_TIMEOUT_SECONDS 30
const long MAX_FILE_SIZE_MB = 10;
const long MAX_FILE_SIZE_BYTES = MAX_FILE_SIZE_MB * 1024 * 1024;
const size_t READ_BUFFER_SIZE = 4096;

WebServer server(80);

#define NUM_SD_CARDS 2
struct SDCardInfo {
  uint8_t cs_pin;
  bool is_mounted;
  uint64_t total_size;
  uint64_t used_size;
  uint8_t card_type;
  String card_name;
};

SDCardInfo sd_cards[NUM_SD_CARDS] = {
  {SD1_CS, false, 0, 0, CARD_NONE, "SD1"},
  {SD2_CS, false, 0, 0, CARD_NONE, "SD2"}
};

struct RAIDFileInfo {
  String filename;
  int primary_card;
  size_t size;
  time_t last_modified;
};

struct FileInfo {
  String name;
  bool isDirectory;
  size_t size;
  String lastModified;
  int storage_card;
};

std::vector<RAIDFileInfo> raid_file_table;
int current_stripe_card = 0;
std::mutex raid_mutex;

// Function declarations
String getIndexPage(String message = "", String messageType = "info");
String getEnhancedFileListHtml(String path);
String getWifiSignalStrengthHtml();
String formatFileSize(size_t bytes);
void handleRoot();

void handleListFiles();
void handleDownload();
void handleUpload();
void handleDelete();
void handleNotFound();
void handleSystemInfo();
void handleSearch();
void handleCreateDirectory();
void handleRenameFile();
bool isAuthenticated();
String sanitizePath(String path);
bool validateFilename(const String& filename);
bool isAllowedFileType(const String& filename);

// RAID 0 functions
bool initRAIDCards();
int getNextStripeCard();
bool writeToRAID(String filename, const uint8_t* data, size_t size, String path = "/");
bool deleteFromRAID(String filename, String path = "/");
uint64_t getTotalRAIDSpace();
uint64_t getUsedRAIDSpace();
void printRAIDStatus();
void rebuildFileTable();
String getRAIDStatusHtml();
void switchSDCard(int card_index);
String getFullPath(String filename, String path);
void addToFileTable(String filename, int card_index, size_t size);
void scanDirectory(const char* dir_path, int card_index);
bool checkCardHealth(int card_index);
bool validateFileSize(size_t size);
void diagnoseSDCard(int card_index);
size_t calculateDirectorySize(const char* dirPath);
bool deleteDirectory(const char* dirPath, int card_index);

// Upload tracking
bool isUploading = false;
unsigned long uploadedBytes = 0;
unsigned long totalUploadSize = 0;
String currentUploadFilename = "";
String currentUploadPath = "";
std::vector<uint8_t> uploadBuffer;

// Input sanitization
String sanitizePath(String path) {
  path.trim();
  while (path.indexOf("..") != -1) {
    path.replace("..", "");
  }
  while (path.indexOf("//") != -1) {
    path.replace("//", "/");
  }
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  if (path.length() > 1 && path.endsWith("/")) {
    path.remove(path.length() - 1);
  }
  return path;
}

bool validateFilename(const String& filename) {
  if (filename.length() == 0 || filename.length() > 255) return false;
  
  const char* forbiddenChars = "<>:\"|?*\\";
  for (int i = 0; i < filename.length(); i++) {
    if (strchr(forbiddenChars, filename.charAt(i)) != NULL) {
      return false;
    }
  }
  
  return true;
}

bool validateFileSize(size_t size) {
  return size <= MAX_FILE_SIZE_BYTES;
}

void diagnoseSDCard(int card_index) {
  if (card_index < 0 || card_index >= NUM_SD_CARDS) return;
  
  Serial.printf("\n=== Diagnosing %s (Card %d) ===\n", sd_cards[card_index].card_name.c_str(), card_index);
  
  switchSDCard(card_index);
  
  uint8_t cardType = SD.cardType();
  Serial.printf("Card type: %d\n", cardType);
  
  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected");
    return;
  }
  
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }
  
  if (!root.isDirectory()) {
    Serial.println("Root is not a directory");
    root.close();
    return;
  }
  
  Serial.println("Root directory opened successfully");
  Serial.println("Listing files:");
  
  File file = root.openNextFile();
  int fileCount = 0;
  while (file) {
    fileCount++;
    if (file.isDirectory()) {
      Serial.printf("  DIR : %s\n", file.name());
    } else {
      Serial.printf("  FILE: %s (Size: %d bytes)\n", file.name(), file.size());
    }
    file = root.openNextFile();
  }
  
  if (fileCount == 0) {
    Serial.println("  (No files found)");
  }
  
  root.close();
  
  File testFile = SD.open("/test.txt", FILE_WRITE);
  if (testFile) {
    Serial.println("  ✓ Can write files");
    testFile.println("Test data");
    testFile.close();
    
    testFile = SD.open("/test.txt");
    if (testFile) {
      Serial.println("  ✓ Can read files");
      testFile.close();
    }
    
    if (SD.remove("/test.txt")) {
      Serial.println("  ✓ Can delete files");
    }
  } else {
    Serial.println("  ✗ Cannot write files");
  }
  
  Serial.println("===========================\n");
}

bool initRAIDCards() {
  Serial.println("\n=== Initializing RAID 0 (Striping) with 2 SD Cards ===");
  
  bool all_mounted = true;
  
  // Reset SPI bus
  SPI.end();
  delay(100);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  
  // Configure SPI settings
  SPI.setFrequency(4000000); // Start with 4MHz
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  
  delay(200);
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    Serial.printf("\nInitializing %s on CS pin %d\n", 
                  sd_cards[i].card_name.c_str(), sd_cards[i].cs_pin);
    
    pinMode(sd_cards[i].cs_pin, OUTPUT);
    digitalWrite(sd_cards[i].cs_pin, HIGH);
    delay(50);
    
    uint32_t frequencies[] = {1000000, 2000000, 4000000, 8000000, 10000000};
    bool mounted = false;
    
    for (uint32_t freq : frequencies) {
      Serial.printf("  Trying %d Hz... ", freq);
      
      SPI.setFrequency(freq);
      digitalWrite(sd_cards[i].cs_pin, HIGH);
      delay(10);
      digitalWrite(sd_cards[i].cs_pin, LOW);
      delay(10);
      digitalWrite(sd_cards[i].cs_pin, HIGH);
      delay(10);
      
      if (SD.begin(sd_cards[i].cs_pin, SPI, freq)) {
        Serial.println("SUCCESS");
        mounted = true;
        break;
      }
      Serial.println("FAILED");
      delay(100);
    }
    
    if (mounted) {
      sd_cards[i].is_mounted = true;
      switchSDCard(i);
      sd_cards[i].card_type = SD.cardType();
      sd_cards[i].total_size = SD.cardSize();
      sd_cards[i].used_size = 0;
      
      Serial.printf("  ✓ %s mounted successfully\n", sd_cards[i].card_name.c_str());
      
      if (sd_cards[i].total_size > 0) {
        Serial.printf("    Total Size: %llu MB\n", sd_cards[i].total_size / (1024 * 1024));
        
        // Test basic operations
        diagnoseSDCard(i);
      } else {
        Serial.println("    Warning: Could not determine card size");
      }
    } else {
      sd_cards[i].is_mounted = false;
      all_mounted = false;
      Serial.printf("  ✗ %s FAILED to mount\n", sd_cards[i].card_name.c_str());
    }
    
    digitalWrite(sd_cards[i].cs_pin, HIGH);
    delay(10);
  }
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      Serial.printf("\nDefault card set to: %s\n", sd_cards[i].card_name.c_str());
      break;
    }
  }
  
  rebuildFileTable();
  
  return all_mounted;
}

int getNextStripeCard() {
  std::lock_guard<std::mutex> lock(raid_mutex);
  
  int attempts = 0;
  int card = current_stripe_card;
  
  do {
    card = (card + 1) % NUM_SD_CARDS;
    attempts++;
    
    if (attempts >= NUM_SD_CARDS * 2) {
      Serial.println("Warning: No suitable card found");
      return -1;
    }
    
    if(sd_cards[card].is_mounted) {
      if (!checkCardHealth(card)) {
        Serial.printf("Card %d failed health check\n", card);
        continue;
      }
      
      uint64_t free_space = sd_cards[card].total_size - sd_cards[card].used_size;
      if(free_space > 1024 * 1024) {
        current_stripe_card = card;
        return card;
      } else {
        Serial.printf("Card %d has insufficient free space\n", card);
      }
    }
  } while(true);
  
  return -1;
}

void switchSDCard(int card_index) {
  if(card_index < 0 || card_index >= NUM_SD_CARDS) return;
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    digitalWrite(sd_cards[i].cs_pin, HIGH);
  }
  delayMicroseconds(100);
  digitalWrite(sd_cards[card_index].cs_pin, LOW);
  delayMicroseconds(100);
}

bool checkCardHealth(int card_index) {
  if (card_index < 0 || card_index >= NUM_SD_CARDS) return false;
  if (!sd_cards[card_index].is_mounted) return false;
  
  switchSDCard(card_index);
  File testFile = SD.open("/health.tmp", FILE_WRITE);
  if (!testFile) {
    Serial.printf("Card %d: Failed to write\n", card_index);
    return false;
  }
  
  const char* testData = "Health Check";
  testFile.write((uint8_t*)testData, strlen(testData));
  testFile.close();
  
  testFile = SD.open("/health.tmp", FILE_READ);
  if (!testFile) {
    Serial.printf("Card %d: Failed to read\n", card_index);
    return false;
  }
  
  char buffer[20];
  size_t read = testFile.read((uint8_t*)buffer, sizeof(buffer));
  testFile.close();
  SD.remove("/health.tmp");
  
  bool healthy = (read == strlen(testData));
  if (!healthy) {
    Serial.printf("Card %d: Data corrupted\n", card_index);
  }
  
  return healthy;
}

bool writeToRAID(String filename, const uint8_t* data, size_t size, String path) {
  Serial.printf("\n=== Writing to RAID ===\n");
  Serial.printf("File: %s, Path: %s, Size: %d bytes\n", filename.c_str(), path.c_str(), size);
  
  if (!validateFilename(filename)) {
    Serial.println("Invalid filename");
    return false;
  }
  
  if (!validateFileSize(size)) {
    Serial.println("File too large");
    return false;
  }
  
  std::lock_guard<std::mutex> lock(raid_mutex);
  
  int target_card = getNextStripeCard();
  if(target_card < 0) {
    Serial.println("No SD cards available");
    return false;
  }
  
  Serial.printf("Selected card: %s (Card %d)\n", sd_cards[target_card].card_name.c_str(), target_card);
  
  String full_path = getFullPath(filename, path);
  full_path = sanitizePath(full_path);
  
  Serial.printf("Full path: %s\n", full_path.c_str());
  
  switchSDCard(target_card);
  
  int lastSlash = full_path.lastIndexOf('/');
  if(lastSlash > 0) {
    String dir_path = full_path.substring(0, lastSlash);
    Serial.printf("Creating directories: %s\n", dir_path.c_str());
    
    String current_dir = "";
    int start_pos = 0;
    while((start_pos = dir_path.indexOf('/', start_pos)) != -1) {
      current_dir = dir_path.substring(0, start_pos);
      if(!SD.exists(current_dir)) {
        Serial.printf("  Creating: %s\n", current_dir.c_str());
        if(!SD.mkdir(current_dir)) {
          Serial.printf("  Failed to create: %s\n", current_dir.c_str());
          switchSDCard(0);
          return false;
        }
      }
      start_pos++;
    }
  }
  
  Serial.printf("Opening file for writing...\n");
  File file = SD.open(full_path, FILE_WRITE);
  if(!file) {
    Serial.printf("Failed to open file\n");
    switchSDCard(0);
    return false;
  }
  
  size_t bytes_written = file.write(data, size);
  file.close();
  
  Serial.printf("Bytes written: %d/%d\n", bytes_written, size);
  
  if (bytes_written != size) {
    Serial.println("Write incomplete");
    if (SD.exists(full_path)) {
      SD.remove(full_path);
    }
    switchSDCard(0);
    return false;
  }
  

  
  if (SD.exists(full_path)) {
    File verifyFile = SD.open(full_path, FILE_READ);
    if (verifyFile) {
      size_t fileSize = verifyFile.size();
      verifyFile.close();
      Serial.printf("File verified: exists, size: %d bytes\n", fileSize);
    }
  } else {
    Serial.printf("ERROR: File doesn't exist after write!\n");
  }
  
  addToFileTable(full_path, target_card, bytes_written);
  sd_cards[target_card].used_size += bytes_written;
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      break;
    }
  }
  
  Serial.printf("✓ Successfully wrote %d bytes\n", bytes_written);
  Serial.println("=== Write completed ===\n");
  
  return true;
}

bool deleteDirectory(const char* dirPath, int card_index) {
  switchSDCard(card_index);
  
  File root = SD.open(dirPath);
  if(!root || !root.isDirectory()) {
    switchSDCard(0);
    return false;
  }
  
  root.rewindDirectory();
  File file = root.openNextFile();
  while(file) {
    String filePath = file.name();
    filePath = sanitizePath(filePath);
    
    if(file.isDirectory()) {
      String dirName = filePath;
      if(dirName != "." && dirName != ".." && dirName != String(dirPath)) {
        deleteDirectory(filePath.c_str(), card_index);
      }
    } else {
      SD.remove(filePath);
      // Remove from file table
      for(auto it = raid_file_table.begin(); it != raid_file_table.end(); ++it) {
        if(it->filename == filePath) {
          sd_cards[card_index].used_size -= it->size;
          raid_file_table.erase(it);
          break;
        }
      }
    }
    file = root.openNextFile();
  }
  root.close();
  
  bool result = SD.rmdir(dirPath);
  switchSDCard(0);
  return result;
}

bool deleteFromRAID(String filename, String path) {
  std::lock_guard<std::mutex> lock(raid_mutex);
  
  String full_path = getFullPath(filename, path);
  full_path = sanitizePath(full_path);
  
  // First check if it's a directory
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      File testFile = SD.open(full_path);
      if(testFile) {
        if(testFile.isDirectory()) {
          testFile.close();
          bool result = deleteDirectory(full_path.c_str(), i);
          if(result) {
            sd_cards[i].used_size = calculateDirectorySize("/");
            rebuildFileTable();
            switchSDCard(0);
            return true;
          }
        } else {
          testFile.close();
        }
      }
    }
  }
  
  // Try to delete as file from file table
  for(auto it = raid_file_table.begin(); it != raid_file_table.end(); ++it) {
    if(it->filename == full_path) {
      int card_index = it->primary_card;
      switchSDCard(card_index);
      
      if(SD.remove(full_path)) {
        sd_cards[card_index].used_size -= it->size;
        raid_file_table.erase(it);
        
        switchSDCard(0);
        return true;
      }
      
      switchSDCard(0);
      return false;
    }
  }
  
  // Try to delete from any card
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      if(SD.exists(full_path)) {
        if(SD.remove(full_path)) {
          sd_cards[i].used_size = calculateDirectorySize("/");
          rebuildFileTable();
          
          switchSDCard(0);
          return true;
        }
      }
    }
  }
  switchSDCard(0);
  return false;
}

void addToFileTable(String filename, int card_index, size_t size) {
  for(auto& file_info : raid_file_table) {
    if(file_info.filename == filename) {
      file_info.size = size;
      file_info.primary_card = card_index;
      file_info.last_modified = time(nullptr);
      return;
    }
  }
  
  RAIDFileInfo new_file;
  new_file.filename = filename;
  new_file.primary_card = card_index;
  new_file.size = size;
  new_file.last_modified = time(nullptr);
  raid_file_table.push_back(new_file);
}

void scanDirectory(const char* dir_path, int card_index) {
  switchSDCard(card_index);
  File root = SD.open(dir_path);
  if(!root || !root.isDirectory()) {
    switchSDCard(0);
    return;
  }
  
  File file = root.openNextFile();
  while(file) {
    if(!file.isDirectory()) {
      String full_path = file.name();
      addToFileTable(full_path, card_index, file.size());
    } else {
      String dir_name = file.name();
      if(dir_name != "." && dir_name != "..") {
        scanDirectory(file.name(), card_index);
      }
    }
    file = root.openNextFile();
  }
  switchSDCard(0);
}

void rebuildFileTable() {
  std::lock_guard<std::mutex> lock(raid_mutex);
  
  raid_file_table.clear();
  Serial.println("Rebuilding RAID file table...");
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      scanDirectory("/", i);
      sd_cards[i].used_size = calculateDirectorySize("/");
    }
  }
  switchSDCard(0);
  Serial.printf("File table rebuilt: %d files tracked\n", raid_file_table.size());
}

uint64_t getTotalRAIDSpace() {
  uint64_t total = 0;
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      total += sd_cards[i].total_size;
    }
  }
  return total;
}

uint64_t getUsedRAIDSpace() {
  uint64_t used = 0;
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      used += sd_cards[i].used_size;
    }
  }
  return used;
}

String getFullPath(String filename, String path) {
  String full_path = sanitizePath(path);
  if(!full_path.endsWith("/") && !filename.startsWith("/")) {
    full_path += "/";
  }
  full_path += filename;
  if(!full_path.startsWith("/")) {
    full_path = "/" + full_path;
  }
  return sanitizePath(full_path);
}

size_t calculateDirectorySize(const char* dirPath) {
  size_t totalSize = 0;
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      
      if (!SD.exists(dirPath)) {
        continue;
      }
      
      File root = SD.open(dirPath);
      if(!root) {
        continue;
      }
      
      if(!root.isDirectory()) {
        root.close();
        continue;
      }
      
      File file = root.openNextFile();
      while(file) {
        if(!file.isDirectory()) {
          totalSize += file.size();
        } else {
          String fileName = file.name();
          if(fileName != "." && fileName != "..") {
            String subDirPath;
            if(strcmp(dirPath, "/") == 0) {
              subDirPath = "/" + fileName;
            } else {
              subDirPath = String(dirPath) + "/" + fileName;
            }
            totalSize += calculateDirectorySize(subDirPath.c_str());
          }
        }
        file = root.openNextFile();
      }
      root.close();
    }
  }
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      break;
    }
  }
  
  return totalSize;
}

void printRAIDStatus() {
  Serial.println("\n=== RAID 0 STATUS ===");
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    Serial.printf("Card %d (%s):\n", i, sd_cards[i].card_name.c_str());
    Serial.printf("  Mounted: %s\n", sd_cards[i].is_mounted ? "Yes" : "No");
    if(sd_cards[i].is_mounted) {
      Serial.printf("  Total: %lluMB\n", sd_cards[i].total_size / (1024 * 1024));
      Serial.printf("  Used: %lluMB\n", sd_cards[i].used_size / (1024 * 1024));
      Serial.printf("  Free: %lluMB\n", 
                   (sd_cards[i].total_size - sd_cards[i].used_size) / (1024 * 1024));
    }
  }
  
  Serial.printf("\nRAID Totals:\n");
  Serial.printf("  Total Space: %lluMB\n", getTotalRAIDSpace() / (1024 * 1024));
  Serial.printf("  Used Space: %lluMB\n", getUsedRAIDSpace() / (1024 * 1024));
  Serial.printf("  Available: %lluMB\n", 
               (getTotalRAIDSpace() - getUsedRAIDSpace()) / (1024 * 1024));
  Serial.printf("  Files Tracked: %d\n", raid_file_table.size());
  Serial.println("=====================\n");
}

String getRAIDStatusHtml() {
  String html = R"rawliteral(
  <div class="mb-6 p-4 bg-gradient-to-r from-purple-50 to-indigo-50 border border-purple-200 rounded-lg">
    <h3 class="text-lg font-semibold text-gray-800 mb-3">RAID 0 Configuration (Striping)</h3>
    <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
  )rawliteral";
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    String status_color = sd_cards[i].is_mounted ? "text-green-600" : "text-red-600";
    String status_text = sd_cards[i].is_mounted ? "✓ Online" : "✗ Offline";
    
    html += "<div class='p-3 bg-white rounded-lg border border-gray-200'>";
    html += "<div class='flex justify-between items-center mb-2'>";
    html += "<span class='font-medium text-gray-700'>" + sd_cards[i].card_name + "</span>";
    html += "<span class='text-sm font-semibold " + status_color + "'>" + status_text + "</span>";
    html += "</div>";
    
    if(sd_cards[i].is_mounted) {
      html += "<div class='space-y-1 text-sm text-gray-600'>";
      html += "<div class='flex justify-between'><span>Total:</span><span>" + 
              formatFileSize(sd_cards[i].total_size) + "</span></div>";
      html += "<div class='flex justify-between'><span>Used:</span><span>" + 
              formatFileSize(sd_cards[i].used_size) + "</span></div>";
      html += "<div class='flex justify-between'><span>Free:</span><span>" + 
              formatFileSize(sd_cards[i].total_size - sd_cards[i].used_size) + "</span></div>";
      html += "</div>";
      
      float usage_percent = sd_cards[i].total_size > 0 ? 
                           (float)sd_cards[i].used_size / sd_cards[i].total_size * 100 : 0;
      html += "<div class='mt-2'>";
      html += "<div class='w-full bg-gray-200 rounded-full h-2'>";
      html += "<div class='bg-blue-500 h-2 rounded-full' style='width: " + String(usage_percent) + "%;'></div>";
      html += "</div>";
      html += "<div class='text-xs text-gray-500 mt-1 text-right'>" + 
              String(usage_percent, 1) + "% used</div>";
      html += "</div>";
    }
    html += "</div>";
  }
  
  html += "<div class='md:col-span-2 p-3 bg-white rounded-lg border border-gray-200'>";
  html += "<div class='grid grid-cols-2 gap-4 text-sm'>";
  html += "<div class='text-center p-2 bg-gray-50 rounded'>";
  html += "<div class='text-gray-500'>Total RAID Space</div>";
  html += "<div class='text-lg font-semibold text-gray-800'>" + formatFileSize(getTotalRAIDSpace()) + "</div>";
  html += "</div>";
  html += "<div class='text-center p-2 bg-gray-50 rounded'>";
  html += "<div class='text-gray-500'>Files Tracked</div>";
  html += "<div class='text-lg font-semibold text-gray-800'>" + String(raid_file_table.size()) + "</div>";
  html += "</div>";
  html += "<div class='text-center p-2 bg-gray-50 rounded'>";
  html += "<div class='text-gray-500'>Current Striping Card</div>";
  html += "<div class='text-lg font-semibold text-gray-800'>" + sd_cards[current_stripe_card].card_name + "</div>";
  html += "</div>";
  html += "<div class='text-center p-2 bg-gray-50 rounded'>";
  html += "<div class='text-gray-500'>Available Space</div>";
  html += "<div class='text-lg font-semibold text-green-600'>" + 
          formatFileSize(getTotalRAIDSpace() - getUsedRAIDSpace()) + "</div>";
  html += "</div>";
  html += "</div></div></div></div>";
  
  return html;
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println("\n=== ESP32 NAS with RAID 0 (2 SD Cards) ===\n");
  
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
    retries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n✗ WiFi connection failed");
    delay(5000);
    ESP.restart();
  }
  
  Serial.println("\nInitializing SD cards...");
  if(!initRAIDCards()) {
    Serial.println("Warning: Some SD cards failed to initialize");
  }
  
  printRAIDStatus();
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/list", HTTP_GET, handleListFiles);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/upload", HTTP_POST, []() {
    server.send(200, "text/plain", "Upload handler ready");
  }, handleUpload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/system", HTTP_GET, handleSystemInfo);
  server.on("/search", HTTP_GET, handleSearch);
  server.on("/mkdir", HTTP_POST, handleCreateDirectory);
  server.on("/rename", HTTP_POST, handleRenameFile);
  server.on("/debug", HTTP_GET, []() {
    if(!isAuthenticated()) return;
    
    String response = "=== ESP32 NAS Debug Info ===\n\n";
    response += "SD Cards Status:\n";
    for(int i = 0; i < NUM_SD_CARDS; i++) {
      response += String("Card ") + i + " (" + sd_cards[i].card_name + "): ";
      response += sd_cards[i].is_mounted ? "MOUNTED\n" : "NOT MOUNTED\n";
      if(sd_cards[i].is_mounted) {
        response += "  Size: " + String(sd_cards[i].total_size) + " bytes\n";
        response += "  Used: " + String(sd_cards[i].used_size) + " bytes\n";
      }
    }
    response += "\nFiles in RAID table: " + String(raid_file_table.size()) + "\n";
    for(const auto& file : raid_file_table) {
      response += "  " + file.filename + " (" + String(file.size) + " bytes) on " + 
                 sd_cards[file.primary_card].card_name + "\n";
    }
    
    server.send(200, "text/plain", response);
  });
  
  server.on("/testupload", HTTP_GET, []() {
    if(!isAuthenticated()) return;
    
    String html = "<!DOCTYPE html><html><body>";
    html += "<h1>Test Upload</h1>";
    html += "<form action='/upload' method='post' enctype='multipart/form-data'>";
    html += "<input type='file' name='file'><br><br>";
    html += "<input type='submit' value='Upload'>";
    html += "</form>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  });
  
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("✓ HTTP server started");
  
  Serial.println("\n=== Testing file operations ===");
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      Serial.printf("Testing %s...\n", sd_cards[i].card_name.c_str());
      
      File testFile = SD.open("/esp32_test.txt", FILE_WRITE);
      if(testFile) {
        testFile.println("Test file created by ESP32 NAS");
        testFile.close();
        Serial.println("  ✓ Created test file");
        
        testFile = SD.open("/esp32_test.txt");
        if(testFile) {
          Serial.println("  ✓ Can read test file");
          testFile.close();
        }
        
        SD.remove("/esp32_test.txt");
        Serial.println("  ✓ Cleaned up test file");
      } else {
        Serial.printf("  ✗ Cannot create files on %s\n", sd_cards[i].card_name.c_str());
      }
    }
  }
  
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      break;
    }
  }
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  
  static unsigned long lastMaintenance = 0;
  if (millis() - lastMaintenance > 60000) {
    lastMaintenance = millis();
    
    for(int i = 0; i < NUM_SD_CARDS; i++) {
      if(sd_cards[i].is_mounted) {
        if (!checkCardHealth(i)) {
          Serial.printf("Card %d failed health check\n", i);
          sd_cards[i].is_mounted = false;
        }
      }
    }
  }
}

void handleUpload() {
  esp_task_wdt_reset();
  
  if (!isAuthenticated()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    isUploading = true;
    uploadedBytes = 0;
    totalUploadSize = upload.totalSize;
    uploadBuffer.clear();
    uploadBuffer.reserve(totalUploadSize > 0 ? totalUploadSize : 1024);
    
    currentUploadFilename = upload.filename;
    currentUploadPath = server.hasArg("path") ? server.arg("path") : "/";
    
    Serial.printf("Upload Start: %s (%.2f MB)\n", 
                  currentUploadFilename.c_str(), (float)totalUploadSize / (1024 * 1024));
    
    if (!validateFilename(currentUploadFilename)) {
      server.send(400, "text/plain", "Invalid filename");
      isUploading = false;
      uploadBuffer.clear();
      return;
    }
    
    if (!validateFileSize(totalUploadSize)) {
      server.send(413, "text/plain", "File too large. Max " + String(MAX_FILE_SIZE_MB) + "MB");
      isUploading = false;
      uploadBuffer.clear();
      return;
    }
    
    if (!isAllowedFileType(currentUploadFilename)) {
      server.send(415, "text/plain", "File type not allowed");
      isUploading = false;
      uploadBuffer.clear();
      return;
    }
    
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    
    // Accumulate data in buffer
    size_t oldSize = uploadBuffer.size();
    uploadBuffer.resize(oldSize + upload.currentSize);
    memcpy(uploadBuffer.data() + oldSize, upload.buf, upload.currentSize);
    
    uploadedBytes += upload.currentSize;
    if (totalUploadSize > 0) {
      unsigned long percent = (unsigned long)((double)uploadedBytes / totalUploadSize * 100);
      Serial.printf("Upload Progress: %lu%% (%lu/%lu bytes)\n", percent, uploadedBytes, totalUploadSize);
    }
    
  } else if (upload.status == UPLOAD_FILE_END) {
    esp_task_wdt_reset();
    
    Serial.printf("Upload Complete: %s (%lu bytes), writing to RAID...\n", 
                  currentUploadFilename.c_str(), uploadedBytes);
    
    // Write accumulated data to RAID
    if (!writeToRAID(currentUploadFilename, uploadBuffer.data(), uploadBuffer.size(), currentUploadPath)) {
      server.send(500, "text/plain", "Upload failed: Could not write to storage");
      isUploading = false;
      uploadBuffer.clear();
      currentUploadFilename = "";
      currentUploadPath = "";
      return;
    }
    
    server.send(200, "text/plain", "File uploaded successfully!");
    Serial.println("Upload successful!");
    
    isUploading = false;
    uploadedBytes = 0;
    totalUploadSize = 0;
    uploadBuffer.clear();
    currentUploadFilename = "";
    currentUploadPath = "";
    rebuildFileTable();
    
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("Upload aborted");
    server.send(500, "text/plain", "Upload aborted");
    isUploading = false;
    uploadedBytes = 0;
    totalUploadSize = 0;
    uploadBuffer.clear();
    currentUploadFilename = "";
    currentUploadPath = "";
  }
}

void handleDownload() {
  if (!isAuthenticated()) return;
  
  String fileName = server.arg("file");
  String path = server.hasArg("path") ? server.arg("path") : "/";
  
  if (!fileName || !validateFilename(fileName)) {
    server.send(400, "text/plain", "Invalid file name");
    return;
  }
  
  String full_path = getFullPath(fileName, path);
  full_path = sanitizePath(full_path);
  size_t file_size = 0;
  int card_index = -1;
  
  // First try to find in file table
  for(const auto& file_info : raid_file_table) {
    if(file_info.filename == full_path) {
      file_size = file_info.size;
      card_index = file_info.primary_card;
      break;
    }
  }
  
  // If not found in table, search all cards
  if (card_index == -1) {
    for(int i = 0; i < NUM_SD_CARDS; i++) {
      if(sd_cards[i].is_mounted) {
        switchSDCard(i);
        if(SD.exists(full_path)) {
          File testFile = SD.open(full_path, FILE_READ);
          if(testFile && !testFile.isDirectory()) {
            file_size = testFile.size();
            card_index = i;
            testFile.close();
            // Add to file table for future reference
            addToFileTable(full_path, i, file_size);
            break;
          }
          if(testFile) testFile.close();
        }
      }
    }
  }
  
  if (card_index == -1) {
    switchSDCard(0);
    server.send(404, "text/plain", "File not found: " + full_path);
    return;
  }
  
  switchSDCard(card_index);
  File file = SD.open(full_path, FILE_READ);
  if (!file || file.isDirectory()) {
    switchSDCard(0);
    server.send(404, "text/plain", "File not found or is directory");
    return;
  }
  
  file_size = file.size();
  server.setContentLength(file_size);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
  server.send(200, "application/octet-stream", "");
  
  uint8_t buffer[READ_BUFFER_SIZE];
  size_t bytes_read;
  
  while((bytes_read = file.read(buffer, sizeof(buffer))) > 0) {
    esp_task_wdt_reset();
    server.sendContent_P((const char*)buffer, bytes_read);
  }
  
  file.close();
  switchSDCard(0);
}

void handleDelete() {
  if (!isAuthenticated()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  
  String fileName = server.arg("file");
  String path = server.hasArg("path") ? server.arg("path") : "/";
  
  if (!fileName || fileName.length() == 0) {
    server.send(400, "text/plain", "Invalid file/directory name");
    return;
  }
  
  // Allow directory names (they don't need filename validation)
  bool isDirectory = false;
  if (fileName.indexOf('/') == -1 && validateFilename(fileName)) {
    // Valid filename
  } else if (fileName.indexOf('/') == -1) {
    // Might be a directory name
    isDirectory = true;
  } else {
    server.send(400, "text/plain", "Invalid file/directory name");
    return;
  }
  
  String full_path = getFullPath(fileName, path);
  full_path = sanitizePath(full_path);
  
  Serial.printf("Deleting: %s (path: %s)\n", full_path.c_str(), path.c_str());
  
  if (deleteFromRAID(fileName, path)) {
    rebuildFileTable();
    server.send(200, "text/plain", (isDirectory ? "Directory" : "File") + String(" deleted: ") + fileName);
  } else {
    server.send(500, "text/plain", "Failed to delete: " + fileName);
  }
}

void handleSystemInfo() {
  if (!isAuthenticated()) return;
  
  size_t sdUsed = getUsedRAIDSpace();
  size_t sdTotal = getTotalRAIDSpace();
  
  String json = "{";
  json += "\"freeHeap\":" + String(esp_get_free_heap_size()) + ",";
  json += "\"sdUsed\":" + String(sdUsed) + ",";
  json += "\"sdTotal\":" + String(sdTotal) + ",";
  json += "\"wifiStrength\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"raidCards\":" + String(NUM_SD_CARDS) + ",";
  json += "\"filesTracked\":" + String(raid_file_table.size()) + ",";
  json += "\"activeUpload\":" + String(isUploading ? "true" : "false");
  json += "}";
  
  server.send(200, "application/json", json);
}

String getEnhancedFileListHtml(String path) {
  std::lock_guard<std::mutex> lock(raid_mutex);
  
  // Normalize path
  path = sanitizePath(path);
  if (!path.endsWith("/") && path != "/") {
    path += "/";
  }
  if (path == "/") {
    path = "/";
  }
  
  std::vector<FileInfo> files;
  std::vector<String> seenFiles;
  
  // Get files from RAID table
  for(const auto& raid_file : raid_file_table) {
    String file_path = raid_file.filename;
    file_path = sanitizePath(file_path);
    
    // Check if file is in the current directory
    bool isInCurrentDir = false;
    String rel_path = "";
    
    if (path == "/") {
      // For root, file should be directly in root (no subdirectories)
      if (file_path.startsWith("/") && file_path.indexOf('/', 1) == -1) {
        isInCurrentDir = true;
        rel_path = file_path.substring(1);
      }
    } else {
      // For subdirectories, file should start with path and have no more slashes after
      if (file_path.startsWith(path)) {
        rel_path = file_path.substring(path.length());
        if (rel_path.indexOf('/') == -1 && rel_path.length() > 0) {
          isInCurrentDir = true;
        }
      }
    }
    
    if (isInCurrentDir && rel_path.length() > 0) {
      // Check if we've already added this file
      bool alreadyAdded = false;
      for(const auto& seen : seenFiles) {
        if(seen == rel_path) {
          alreadyAdded = true;
          break;
        }
      }
      
      if (!alreadyAdded) {
        FileInfo info;
        info.name = rel_path;
        info.isDirectory = false;
        info.size = raid_file.size;
        info.lastModified = "N/A";
        info.storage_card = raid_file.primary_card;
        
        files.push_back(info);
        seenFiles.push_back(rel_path);
      }
    }
  }
  
  // Get directories from SD cards
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      File root = SD.open(path);
      if(root && root.isDirectory()) {
        root.rewindDirectory();
        File file = root.openNextFile();
        while(file) {
          String item_name = file.name();
          // Extract just the name part
          if(item_name.lastIndexOf('/') != -1) {
            item_name = item_name.substring(item_name.lastIndexOf('/') + 1);
          }
          
          if(item_name != "." && item_name != "..") {
            bool exists = false;
            for(const auto& f : files) {
              if(f.name == item_name) {
                exists = true;
                break;
              }
            }
            if(!exists) {
              FileInfo info;
              info.name = item_name;
              info.isDirectory = file.isDirectory();
              info.size = file.isDirectory() ? 0 : file.size();
              info.lastModified = "N/A";
              info.storage_card = i;
              files.push_back(info);
              seenFiles.push_back(item_name);
            }
          }
          file = root.openNextFile();
        }
        root.close();
      }
    }
  }
  
  switchSDCard(0);
  
  std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
    if(a.isDirectory != b.isDirectory) {
      return a.isDirectory > b.isDirectory;
    }
    return a.name < b.name;
  });
  
  String html = R"rawliteral(
  <div class="overflow-x-auto">
    <table class="min-w-full bg-white rounded-lg overflow-hidden">
      <thead>
        <tr class="bg-gray-100">
          <th class="py-3 px-4 text-left text-sm font-semibold text-gray-700">Name</th>
          <th class="py-3 px-4 text-left text-sm font-semibold text-gray-700">Size</th>
          <th class="py-3 px-4 text-left text-sm font-semibold text-gray-700">Location</th>
          <th class="py-3 px-4 text-left text-sm font-semibold text-gray-700">Modified</th>
          <th class="py-3 px-4 text-left text-sm font-semibold text-gray-700">Actions</th>
        </tr>
      </thead>
      <tbody>
  )rawliteral";
  
  if (path != "/") {
    html += R"rawliteral(
    <tr class='border-b border-gray-200 hover:bg-gray-50 cursor-pointer' onclick='navigateTo("..")'>
      <td class='py-3 px-4'>
        <div class='flex items-center text-blue-600 font-medium'>
          <svg class='w-5 h-5 mr-2' fill='none' stroke='currentColor' viewBox='0 0 24 24'>
            <path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6'/>
          </svg>
          ..
        </div>
      </td>
      <td class='py-3 px-4 text-sm text-gray-500'>-</td>
      <td class='py-3 px-4 text-sm text-gray-500'>-</td>
      <td class='py-3 px-4 text-sm text-gray-500'>-</td>
      <td class='py-3 px-4 text-sm text-gray-500'>-</td>
    </tr>
    )rawliteral";
  }
  
  for (const auto& info : files) {
    html += "<tr class='border-b border-gray-200 hover:bg-gray-50'>";
    html += "<td class='py-3 px-4'>";
    if (info.isDirectory) {
      html += "<div class='flex items-center text-blue-600 font-medium cursor-pointer' onclick='navigateTo(\"" + info.name + "\")'>";
      html += "<svg class='w-5 h-5 mr-2' fill='none' stroke='currentColor' viewBox='0 0 24 24'>";
      html += "<path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z'/>";
      html += "</svg>" + info.name + "</div>";
    } else {
      html += "<div class='flex items-center text-gray-700'>";
      html += "<svg class='w-5 h-5 mr-2 text-gray-400' fill='none' stroke='currentColor' viewBox='0 0 24 24'>";
      html += "<path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z'/>";
      html += "</svg>" + info.name + "</div>";
    }
    html += "</td>";
    
    html += "<td class='py-3 px-4 text-sm text-gray-600'>";
    html += info.isDirectory ? "-" : formatFileSize(info.size);
    html += "</td>";
    
    html += "<td class='py-3 px-4 text-sm text-gray-600'>";
    html += sd_cards[info.storage_card].card_name;
    html += "</td>";
    
    html += "<td class='py-3 px-4 text-sm text-gray-600'>" + info.lastModified + "</td>";
    
    html += "<td class='py-3 px-4'>";
    html += "<div class='flex space-x-2'>";
    if (!info.isDirectory) {
      html += "<button onclick='downloadFile(\"" + info.name + "\")' class='bg-blue-500 hover:bg-blue-600 text-white text-xs py-1 px-3 rounded transition duration-150'>Download</button>";
    }
    html += "<button onclick='deleteFile(\"" + info.name + "\")' class='bg-red-500 hover:bg-red-600 text-white text-xs py-1 px-3 rounded transition duration-150'>" + String(info.isDirectory ? "Delete Dir" : "Delete") + "</button>";
    html += "</div></td>";
    
    html += "</tr>";
  }
  
  html += "</tbody></table></div>";
  
  if (files.empty() && path == "/") {
    html = "<div class='text-center py-8'><p class='text-gray-500 text-lg'>No files found. Upload some files to get started!</p></div>";
  }
  
  return html;
}

String getIndexPage(String message, String messageType) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 NAS with RAID 0</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <script src="https://cdn.tailwindcss.com"></script>
    <style>
        body { font-family: 'Inter', sans-serif; }
        .upload-dropzone {
            border: 2px dashed #cbd5e0;
            border-radius: 0.5rem;
            padding: 2rem;
            text-align: center;
            cursor: pointer;
            transition: all 0.3s;
        }
        .upload-dropzone:hover {
            border-color: #4299e1;
            background-color: #ebf8ff;
        }
        .upload-dropzone.dragover {
            border-color: #4299e1;
            background-color: #bee3f8;
        }
    </style>
</head>
<body class="bg-gray-100 p-4 sm:p-8">
    <div class="max-w-6xl mx-auto bg-white p-6 rounded-lg shadow-lg">
        <div class="flex justify-between items-center mb-6 pb-4 border-b border-gray-200">
            <div>
                <h1 class="text-3xl font-bold text-gray-800">ESP32 NAS with RAID 0</h1>
                <p class="text-gray-600">Striping across 2 SD cards</p>
            </div>
            <div class="flex items-center space-x-4">
                <div id="systemStats" class="text-sm text-gray-600">
                    <span id="freeMemory">Loading...</span> | 
                    <span id="storageUsed">Loading...</span> used
                </div>
                <button onclick="refreshSystemInfo()" class="bg-blue-500 hover:bg-blue-600 text-white text-sm py-1 px-3 rounded">
                    Refresh
                </button>
            </div>
        </div>
  )rawliteral";

  if (message != "") {
    String bgColor, borderColor, textColor;
    if (messageType == "success") {
      bgColor = "bg-green-100"; borderColor = "border-green-400"; textColor = "text-green-700";
    } else if (messageType == "error") {
      bgColor = "bg-red-100"; borderColor = "border-red-400"; textColor = "text-red-700";
    } else {
      bgColor = "bg-blue-100"; borderColor = "border-blue-400"; textColor = "text-blue-700";
    }
    
    html += "<div id='messageBox' class='" + bgColor + " " + borderColor + " " + textColor + " px-4 py-3 rounded-lg relative mb-4' role='alert'>";
    html += "<span class='block sm:inline'>" + message + "</span>";
    html += "<span class='absolute top-0 bottom-0 right-0 px-4 py-3 cursor-pointer' onclick=\"document.getElementById('messageBox').style.display='none';\">";
    html += "<svg class='fill-current h-6 w-6 " + textColor + "' role='button' xmlns='http://www.w3.org/2000/svg' viewBox='0 0 20 20'><title>Close</title><path d='M14.348 14.849a1.2 1.2 0 0 1-1.697 0L10 11.819l-2.651 3.029a1.2 1.2 0 1 1-1.697-1.697l2.758-3.15-2.759-3.152a1.2 1.2 0 1 1 1.697-1.697L10 8.183l2.651-3.031a1.2 1.2 0 1 1 1.697 1.697l-2.758 3.152 2.758 3.15a1.2 1.2 0 0 1 0 1.698z'/></svg>";
    html += "</span></div>";
  }

  html += getWifiSignalStrengthHtml();
  html += getRAIDStatusHtml();
  
  html += R"rawliteral(
        <!-- File Upload -->
        <div class="mb-6">
            <h2 class="text-xl font-semibold text-gray-800 mb-4">Upload Files</h2>
            <div class="bg-gray-50 p-4 rounded-lg border border-gray-200">
                <div class="upload-dropzone" id="uploadDropzone" onclick="document.getElementById('fileInput').click()">
                    <div class="py-4">
                        <svg class="w-12 h-12 mx-auto text-gray-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" 
                                  d="M7 16a4 4 0 01-.88-7.903A5 5 0 1115.9 6L16 6a5 5 0 011 9.9M15 13l-3-3m0 0l-3 3m3-3v12"/>
                        </svg>
                        <p class="mt-2 text-gray-600">Click to select files or drag and drop</p>
                        <p class="text-sm text-gray-500">Max 10MB per file</p>
                    </div>
                </div>
                
                <input type="file" id="fileInput" multiple style="display: none;" onchange="handleFileSelect(this.files)">
                
                <div id="uploadProgress" class="mt-4 hidden">
                    <div class="flex justify-between text-sm text-gray-600 mb-1">
                        <span id="uploadStatus">Preparing...</span>
                        <span id="uploadPercentage">0%</span>
                    </div>
                    <div class="w-full bg-gray-200 rounded-full h-2">
                        <div id="uploadProgressBar" class="bg-blue-500 h-2 rounded-full" style="width: 0%"></div>
                    </div>
                </div>
                
                <div id="fileList" class="mt-4 space-y-2 max-h-60 overflow-y-auto"></div>
                
                <div class="mt-4 flex justify-between">
                    <button onclick="startUpload()" id="uploadButton" 
                            class="bg-green-500 hover:bg-green-600 text-white font-medium py-2 px-4 rounded disabled:opacity-50"
                            disabled>
                        Upload Files
                    </button>
                    <button onclick="clearFileList()" class="bg-gray-300 hover:bg-gray-400 text-gray-800 font-medium py-2 px-4 rounded">
                        Clear
                    </button>
                </div>
            </div>
        </div>
        
        <!-- File Browser -->
        <div class="mb-6">
            <div class="flex justify-between items-center mb-4">
                <h2 class="text-xl font-semibold text-gray-800">File Browser</h2>
                <div class="flex space-x-2">
                    <input type="text" id="searchInput" placeholder="Search..." 
                           class="border border-gray-300 rounded-lg px-3 py-1 text-sm w-40"
                           onkeypress="if(event.keyCode==13) searchFiles()">
                    <button onclick="createFolder()" class="bg-purple-500 hover:bg-purple-600 text-white text-sm py-1 px-3 rounded">
                        New Folder
                    </button>
                    <button onclick="refreshFileList()" class="bg-gray-500 hover:bg-gray-600 text-white text-sm py-1 px-3 rounded">
                        Refresh
                    </button>
                </div>
            </div>
            
            <div class="mb-3">
                <div class="flex items-center text-sm text-gray-600 bg-gray-50 p-2 rounded">
                    <span class="font-medium">Path:</span>
                    <span id="currentPath" class="ml-2">/</span>
                </div>
            </div>
            
            <div id="fileListContainer">
                Loading...
            </div>
        </div>
        
        <script>
            let currentPath = '/';
            let selectedFiles = [];
            
            function refreshSystemInfo() {
                fetch('/system')
                    .then(response => response.json())
                    .then(data => {
                        const freeMem = Math.round(data.freeHeap / 1024);
                        const usedStorage = Math.round(data.sdUsed / (1024 * 1024));
                        const totalStorage = Math.round(data.sdTotal / (1024 * 1024));
                        document.getElementById('freeMemory').textContent = freeMem + ' KB';
                        document.getElementById('storageUsed').textContent = usedStorage + ' / ' + totalStorage + ' MB';
                    });
            }
            
            const dropzone = document.getElementById('uploadDropzone');
            
            dropzone.addEventListener('dragover', (e) => {
                e.preventDefault();
                dropzone.classList.add('dragover');
            });
            
            dropzone.addEventListener('dragleave', (e) => {
                e.preventDefault();
                dropzone.classList.remove('dragover');
            });
            
            dropzone.addEventListener('drop', (e) => {
                e.preventDefault();
                dropzone.classList.remove('dragover');
                if (e.dataTransfer.files.length > 0) {
                    handleFileSelect(e.dataTransfer.files);
                }
            });
            
            function handleFileSelect(files) {
                const fileListDiv = document.getElementById('fileList');
                const uploadButton = document.getElementById('uploadButton');
                
                for (let file of files) {
                    if (file.size > 10 * 1024 * 1024) {
                        alert('File too large: ' + file.name);
                        continue;
                    }
                    
                    if (selectedFiles.some(f => f.name === file.name && f.size === file.size)) {
                        continue;
                    }
                    
                    selectedFiles.push(file);
                    
                    const fileItem = document.createElement('div');
                    fileItem.className = 'flex justify-between items-center p-2 bg-white border border-gray-200 rounded';
                    fileItem.innerHTML = `
                        <div class="flex items-center">
                            <svg class="w-5 h-5 text-gray-400 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" 
                                      d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z"/>
                            </svg>
                            <span class="text-sm">${file.name}</span>
                        </div>
                        <div class="text-xs text-gray-500">
                            ${formatFileSize(file.size)}
                        </div>
                    `;
                    fileListDiv.appendChild(fileItem);
                }
                
                if (selectedFiles.length > 0) {
                    uploadButton.disabled = false;
                }
            }
            
            function formatFileSize(bytes) {
                if (bytes === 0) return '0 Bytes';
                const k = 1024;
                const sizes = ['Bytes', 'KB', 'MB', 'GB'];
                const i = Math.floor(Math.log(bytes) / Math.log(k));
                return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
            }
            
            function clearFileList() {
                selectedFiles = [];
                document.getElementById('fileList').innerHTML = '';
                document.getElementById('uploadButton').disabled = true;
            }
            
            async function startUpload() {
                if (selectedFiles.length === 0) return;
                
                const progressDiv = document.getElementById('uploadProgress');
                const progressBar = document.getElementById('uploadProgressBar');
                const uploadStatus = document.getElementById('uploadStatus');
                const uploadPercentage = document.getElementById('uploadPercentage');
                const uploadButton = document.getElementById('uploadButton');
                
                progressDiv.classList.remove('hidden');
                uploadButton.disabled = true;
                
                let totalUploaded = 0;
                let totalSize = selectedFiles.reduce((sum, file) => sum + file.size, 0);
                let uploadedCount = 0;
                
                uploadStatus.textContent = `Uploading ${selectedFiles.length} file(s)...`;
                
                for (let file of selectedFiles) {
                    const formData = new FormData();
                    formData.append('file', file);
                    
                    try {
                        const response = await fetch('/upload?path=' + encodeURIComponent(currentPath), {
                            method: 'POST',
                            body: formData
                        });
                        
                        if (response.ok) {
                            uploadedCount++;
                            totalUploaded += file.size;
                            
                            const percent = Math.round((totalUploaded / totalSize) * 100);
                            progressBar.style.width = percent + '%';
                            uploadPercentage.textContent = percent + '%';
                            uploadStatus.textContent = `Uploading... (${uploadedCount}/${selectedFiles.length})`;
                        } else {
                            const errorText = await response.text();
                            throw new Error(errorText);
                        }
                    } catch (error) {
                        uploadStatus.textContent = 'Upload failed: ' + error.message;
                        uploadStatus.className = 'text-red-600';
                        break;
                    }
                }
                
                if (uploadedCount === selectedFiles.length) {
                    uploadStatus.textContent = 'Upload complete!';
                    uploadStatus.className = 'text-green-600';
                    setTimeout(() => {
                        progressDiv.classList.add('hidden');
                        clearFileList();
                        refreshFileList();
                        refreshSystemInfo();
                    }, 2000);
                }
                
                uploadButton.disabled = false;
            }
            
            function loadFileList(path = '/') {
                currentPath = path;
                document.getElementById('currentPath').textContent = currentPath;
                
                fetch('/list?path=' + encodeURIComponent(path))
                    .then(response => response.text())
                    .then(html => {
                        document.getElementById('fileListContainer').innerHTML = html;
                    });
            }
            
            function refreshFileList() {
                loadFileList(currentPath);
            }
            
            function navigateTo(path) {
                if (path === '..') {
                    const lastSlash = currentPath.lastIndexOf('/');
                    if (lastSlash > 0) {
                        currentPath = currentPath.substring(0, lastSlash);
                    } else {
                        currentPath = '/';
                    }
                } else {
                    if (currentPath === '/') {
                        currentPath = '/' + path;
                    } else {
                        currentPath += '/' + path;
                    }
                }
                loadFileList(currentPath);
            }
            
            function downloadFile(filename) {
                const url = '/download?file=' + encodeURIComponent(filename) + 
                           '&path=' + encodeURIComponent(currentPath);
                window.open(url, '_blank');
            }
            
            function deleteFile(filename) {
                if (!confirm('Delete "' + filename + '"?')) return;
                
                fetch('/delete?file=' + encodeURIComponent(filename) + 
                      '&path=' + encodeURIComponent(currentPath))
                    .then(response => {
                        if (response.ok) {
                            refreshFileList();
                            refreshSystemInfo();
                        }
                    });
            }
            
            function createFolder() {
                const folderName = prompt('Folder name:');
                if (!folderName) return;
                
                fetch('/mkdir', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'name=' + encodeURIComponent(folderName) + 
                          '&path=' + encodeURIComponent(currentPath)
                })
                .then(response => {
                    if (response.ok) {
                        refreshFileList();
                    }
                });
            }
            
            document.addEventListener('DOMContentLoaded', function() {
                loadFileList();
                refreshSystemInfo();
                setInterval(refreshSystemInfo, 30000);
            });
        </script>
    </div>
</body>
</html>
  )rawliteral";
  
  return html;
}

bool isAuthenticated() {
  if (server.authenticate(nas_username, nas_password)) {
    return true;
  }
  server.requestAuthentication();
  return false;
}

String formatFileSize(size_t bytes) {
  const char* sizes[] = {"B", "KB", "MB", "GB"};
  int i = 0;
  double dblBytes = bytes;
  while (dblBytes >= 1024 && i < 3) {
    dblBytes /= 1024;
    i++;
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f %s", dblBytes, sizes[i]);
  return String(buf);
}

String getWifiSignalStrengthHtml() {
  long rssi = WiFi.RSSI();
  String strengthText;
  String colorClass;
  int level;

  if (rssi > -50) {
    strengthText = "Excellent"; colorClass = "text-green-600"; level = 5;
  } else if (rssi > -60) {
    strengthText = "Good"; colorClass = "text-lime-600"; level = 4;
  } else if (rssi > -70) {
    strengthText = "Fair"; colorClass = "text-yellow-600"; level = 3;
  } else if (rssi > -80) {
    strengthText = "Weak"; colorClass = "text-orange-600"; level = 2;
  } else {
    strengthText = "Very Weak"; colorClass = "text-red-600"; level = 1;
  }

  String html = "<div class='mb-6 p-4 rounded-lg bg-gradient-to-r from-blue-50 to-indigo-50 border border-blue-200'>";
  html += "<div class='flex flex-wrap items-center justify-between'>";
  html += "<div class='mb-2 sm:mb-0'>";
  html += "<p class='text-gray-700 font-semibold'>Wi-Fi: <span class='" + colorClass + "'>" + strengthText + " (Level " + String(level) + ")</span></p>";
  html += "<p class='text-sm text-gray-600'>IP: " + WiFi.localIP().toString() + "</p>";
  html += "</div>";
  html += "<div class='text-right'>";
  html += "<p class='text-sm text-gray-600'>Free Memory: <span id='freeMemoryDisplay'>" + String(esp_get_free_heap_size() / 1024) + " KB</span></p>";
  html += "</div>";
  html += "</div></div>";
  return html;
}

void handleRoot() {
  if (!isAuthenticated()) return;
  
  String message = "";
  String messageType = "info";
  if (server.hasArg("message")) {
    message = server.arg("message");
  }
  if (server.hasArg("type")) {
    messageType = server.arg("type");
  }
  server.send(200, "text/html", getIndexPage(message, messageType));
}

void handleListFiles() {
  if (!isAuthenticated()) return;
  
  String path = server.hasArg("path") ? server.arg("path") : "/";
  path = sanitizePath(path);
  
  server.send(200, "text/html", getEnhancedFileListHtml(path));
}

void handleSearch() {
  if (!isAuthenticated()) return;
  
  String query = server.arg("q");
  if (query.length() < 2) {
    server.send(400, "text/plain", "Query too short");
    return;
  }
  
  String html = "<h3 class='text-lg font-semibold mb-4 text-gray-700'>Search Results: \"" + query + "\"</h3>";
  
  std::vector<RAIDFileInfo> results;
  for(const auto& file_info : raid_file_table) {
    String filename = file_info.filename;
    int lastSlash = filename.lastIndexOf('/');
    if(lastSlash != -1) {
      filename = filename.substring(lastSlash + 1);
    }
    if(filename.indexOf(query) != -1) {
      results.push_back(file_info);
    }
  }
  
  if(results.empty()) {
    html += "<p class='text-gray-500 py-4'>No files found.</p>";
  } else {
    html += "<div class='space-y-2'>";
    for(const auto& result : results) {
      String filename = result.filename;
      int lastSlash = filename.lastIndexOf('/');
      String displayName = (lastSlash != -1) ? filename.substring(lastSlash + 1) : filename;
      
      html += "<div class='p-3 bg-white border border-gray-200 rounded-lg flex justify-between items-center'>";
      html += "<div>";
      html += "<span class='text-gray-700'>" + displayName + "</span>";
      html += "<span class='text-xs text-gray-500 ml-2'>(" + sd_cards[result.primary_card].card_name + ")</span>";
      html += "</div>";
      html += "<div class='flex space-x-2'>";
      html += "<button onclick='downloadFile(\"" + displayName + "\")' class='bg-blue-500 hover:bg-blue-600 text-white text-xs py-1 px-3 rounded'>Download</button>";
      html += "</div></div>";
    }
    html += "</div>";
  }

  server.send(200, "text/html", html);
}

void handleCreateDirectory() {
  if (!isAuthenticated()) {
    server.send(401, "text/plain", "Unauthorized.");
    return;
  }
  
  String dirName = server.arg("name");
  String path = server.hasArg("path") ? server.arg("path") : "/";
  
  if (!dirName || !validateFilename(dirName)) {
    server.send(400, "text/plain", "Invalid directory name.");
    return;
  }
  
  String fullPath = sanitizePath(path);
  if (!fullPath.endsWith("/")) fullPath += "/";
  fullPath += dirName;
  
  bool created = false;
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      if (SD.mkdir(fullPath)) {
        created = true;
        break;
      }
    }
  }
  switchSDCard(0);
  
  if (created) {
    server.send(200, "text/plain", "Directory created: " + dirName);
  } else {
    server.send(500, "text/plain", "Failed to create directory.");
  }
}

void handleRenameFile() {
  if (!isAuthenticated()) {
    server.send(401, "text/plain", "Unauthorized.");
    return;
  }
  
  String oldName = server.arg("old");
  String newName = server.arg("new");
  
  if (!oldName || !newName || !validateFilename(oldName) || !validateFilename(newName)) {
    server.send(400, "text/plain", "Invalid file names.");
    return;
  }
  
  String old_path = sanitizePath(oldName);
  String new_path = sanitizePath(newName);
  
  bool renamed = false;
  for(int i = 0; i < NUM_SD_CARDS; i++) {
    if(sd_cards[i].is_mounted) {
      switchSDCard(i);
      if (SD.exists(old_path)) {
        if (SD.rename(old_path, new_path)) {
          renamed = true;
          for(auto& file_info : raid_file_table) {
            if(file_info.filename == old_path) {
              file_info.filename = new_path;
              break;
            }
          }
          break;
        }
      }
    }
  }
  switchSDCard(0);
  
  if (renamed) {
    server.send(200, "text/plain", "File renamed.");
  } else {
    server.send(500, "text/plain", "Failed to rename file.");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not Found");
}

bool isAllowedFileType(const String& filename) {
  const char* allowedExtensions[] = {".txt", ".pdf", ".jpg", ".jpeg", ".png", ".gif", ".mp3", ".mp4", ".doc", ".docx", ".xls", ".xlsx", ".zip", ".rar"};
  for (const char* ext : allowedExtensions) {
    if (filename.endsWith(ext)) {
      return true;
    }
  }
  return false;
}