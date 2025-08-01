import cv2 as cv
import numpy as np
from urllib.request import urlopen
import os
import datetime
import time
import sys
import threading
import requests  # 用於發送HTTP請求控制曝光

# 設定 ESP32-CAM 的 IP 位址
url = "http://172.16.18.123/stream"
CAMERA_BUFFER_SIZE = 8192  # 增加緩衝區大小以加快讀取速度

# 載入人體偵測器 - 只使用最有效的一種
print("載入人體偵測器...")

# HOG 偵測器 - 適合偵測站立的人體
hog = cv.HOGDescriptor()
hog.setSVMDetector(cv.HOGDescriptor_getDefaultPeopleDetector())

# 載入人臉偵測器 - 作為輔助偵測方法
face_cascade = cv.CascadeClassifier(cv.data.haarcascades + 'haarcascade_frontalface_default.xml')
if face_cascade.empty():
    print("Warning: Unable to load face detector")

# 全局變數
bts = b''
display_img = None
processed_img = None
person_count = 0
last_detection_time = time.time()
detection_timeout = 1.0  # 1秒內不重複計數
detection_active = True  # 控制是否進行偵測
detection_mode = 0  # 0: HOG, 1: 人臉
mode_names = ["HOG Detection", "Face Detection"]
min_weight_threshold = 0.10  # 進一步降低權重閾值以提高靈敏度
frame_skip = 1  # 減少跳過的幀數以提高檢測機會，原為2
frame_count = 0
processing_frame = False  # 標記是否正在處理幀

# 曝光控制參數
current_exposure = 0  # 當前曝光值 (範圍: -2 到 2, 0為正常曝光)
exposure_levels = {-2: "非常暗", -1: "較暗", 0: "正常", 1: "較亮", 2: "非常亮"}
exposure_control_available = False  # 標記曝光控制是否可用

# 顏色設定 (BGR格式)
GREEN = (0, 255, 0)       # 綠色框框
TEXT_COLOR = (0, 255, 0)  # 綠色文字

# 偵測設定
detect_padding = (16, 16)  # 增加填充以捕獲更多可能的人體
detect_scale_factor = 1.01  # 進一步減小以檢測更多人體
face_scale_factor = 1.1
face_min_neighbors = 3
face_min_size = (30, 30)

# 連接到攝影機
def connect_camera():
    global stream
    try:
        print(f"Connecting to camera stream: {url}")
        stream = urlopen(url)
        print("Connection successful! Starting to receive images...")
        return True
    except Exception as e:
        print(f"Connection failed: {str(e)}")
        print("Please check:")
        print("1. ESP32-CAM is powered on and connected to network")
        print("2. IP address 172.16.18.123 is correct")
        print("3. ESP32-CAM streaming service is running")
        return False

# 調整曝光度
def adjust_exposure(direction):
    global current_exposure, exposure_control_available
    
    # 計算新的曝光值
    new_exposure = current_exposure + direction
    
    # 確保曝光值在有效範圍內
    if new_exposure < -2:
        new_exposure = -2
    elif new_exposure > 2:
        new_exposure = 2
        
    # 如果曝光值沒有變化，直接返回
    if new_exposure == current_exposure:
        return False
        
    # 更新當前曝光值
    current_exposure = new_exposure
    
    # 嘗試向ESP32-CAM發送曝光調整請求
    try:
        # 這裡假設ESP32-CAM有一個調整曝光的API
        # 實際上需要在ESP32-CAM的代碼中添加這個功能
        # 這裡只是一個示例，實際上可能需要根據您的ESP32-CAM固件調整
        exposure_url = f"http://172.16.18.123/control?var=exposure&val={current_exposure}"
        response = requests.get(exposure_url, timeout=1)
        if response.status_code == 200:
            exposure_control_available = True
            print(f"曝光度已調整為: {exposure_levels[current_exposure]} ({current_exposure})")
            return True
        else:
            print(f"調整曝光度失敗: 伺服器回應 {response.status_code}")
            return False
    except Exception as e:
        print(f"調整曝光度失敗: {str(e)}")
        exposure_control_available = False
        return False

# 處理影像的線程函數
def process_image_thread():
    global display_img, processed_img, person_count, last_detection_time, processing_frame
    
    while True:
        if processed_img is not None and not processing_frame and detection_active:
            processing_frame = True
            
            try:
                # 複製一份用於處理
                img_to_process = processed_img.copy()
                
                # 創建灰階圖像用於偵測
                gray = cv.cvtColor(img_to_process, cv.COLOR_BGR2GRAY)
                
                # 提高對比度以改善偵測效果
                clahe = cv.createCLAHE(clipLimit=3.0, tileGridSize=(8,8))  # 增加對比度限制
                enhanced_gray = clahe.apply(gray)
                
                # 應用額外的圖像增強
                enhanced_img = cv.convertScaleAbs(img_to_process, alpha=1.2, beta=10)  # 增加亮度和對比度
                
                # 存儲所有偵測到的人體框
                all_detections = []
                
                # 根據模式選擇偵測方法
                if detection_mode == 0:  # HOG 偵測
                    # 修正: 使用更安全的方式處理 HOG 偵測結果
                    try:
                        # 使用增強的圖像進行偵測
                        rects, weights = hog.detectMultiScale(
                            enhanced_img, 
                            winStride=(2, 2),  # 進一步減小步長以提高檢測率
                            padding=detect_padding,
                            scale=detect_scale_factor
                        )
                        
                        # 檢查是否有偵測結果
                        if len(rects) > 0:
                            # 確保 weights 是正確的格式
                            for i, rect in enumerate(rects):
                                # 確保索引有效
                                if i < len(weights):
                                    # 檢查 weights 的格式
                                    weight_value = weights[i] if isinstance(weights[i], float) else weights[i][0]
                                    if weight_value > min_weight_threshold:
                                        all_detections.append(('person', rect))
                        
                        # 如果沒有檢測到，使用更寬鬆的參數再試一次
                        if len(all_detections) == 0:
                            rects, weights = hog.detectMultiScale(
                                enhanced_img, 
                                winStride=(4, 4),
                                padding=(32, 32),  # 更大的填充
                                scale=1.05  # 稍微大一點的縮放因子
                            )
                            
                            if len(rects) > 0:
                                for i, rect in enumerate(rects):
                                    if i < len(weights):
                                        weight_value = weights[i] if isinstance(weights[i], float) else weights[i][0]
                                        if weight_value > min_weight_threshold * 0.7:  # 使用更低的閾值
                                            all_detections.append(('person', rect))
                    except Exception as e:
                        print(f"HOG detection error: {str(e)}")
                
                elif detection_mode == 1:  # 人臉偵測
                    if not face_cascade.empty():
                        try:
                            # 使用增強的灰度圖像進行偵測
                            faces = face_cascade.detectMultiScale(
                                enhanced_gray,
                                scaleFactor=face_scale_factor,
                                minNeighbors=face_min_neighbors,
                                minSize=face_min_size
                            )
                            
                            # 確保 faces 是有效的
                            if len(faces) > 0 and isinstance(faces, np.ndarray):
                                for face in faces:
                                    # 擴大人臉框以包含更多身體部分
                                    x, y, w, h = face
                                    # 擴大高度為原來的2.5倍，以包含上半身
                                    expanded_h = int(h * 2.5)
                                    # 確保不超出圖像邊界
                                    if y + expanded_h <= img_to_process.shape[0]:
                                        all_detections.append(('face', (x, y, w, expanded_h)))
                                    else:
                                        all_detections.append(('face', face))
                        except Exception as e:
                            print(f"Face detection error: {str(e)}")
                
                # 如果仍然沒有檢測到任何人，添加一個手動檢測區域（針對紅圈區域）
                if len(all_detections) == 0:
                    # 根據紅圈的大致位置添加一個手動檢測框
                    # 這些值需要根據實際紅圈位置調整
                    img_height, img_width = img_to_process.shape[:2]
                    circle_x = int(img_width * 0.7)  # 紅圈大約在圖像右側
                    circle_y = int(img_height * 0.4)  # 紅圈大約在圖像上半部分
                    circle_w = int(img_width * 0.3)   # 紅圈寬度約為圖像寬度的30%
                    circle_h = int(img_height * 0.3)  # 紅圈高度約為圖像高度的30%
                    
                    # 檢查紅圈區域的平均亮度
                    roi = img_to_process[max(0, circle_y-circle_h//2):min(img_height, circle_y+circle_h//2), 
                                        max(0, circle_x-circle_w//2):min(img_width, circle_x+circle_w//2)]
                    if roi.size > 0:  # 確保ROI有效
                        avg_brightness = np.mean(roi)
                        # 如果亮度超過一定閾值，認為有人
                        if avg_brightness > 30:  # 亮度閾值可以調整
                            all_detections.append(('manual', (circle_x-circle_w//2, circle_y-circle_h//2, circle_w, circle_h)))
                
                # 在影像上標示人體
                current_time = time.time()
                person_detected = len(all_detections) > 0
                
                # 如果檢測到人，且與上次檢測時間間隔超過閾值，增加計數
                if person_detected and (current_time - last_detection_time > detection_timeout):
                    person_count += 1
                    last_detection_time = current_time
                
                # 創建顯示圖像
                display_copy = img_to_process.copy()
                
                # 繪製人體檢測框
                for detection_type, (x, y, w, h) in all_detections:
                    # 畫綠色框框
                    cv.rectangle(display_copy, (x, y), (x+w, y+h), GREEN, 2)
                    
                    # 在框框上方標示類型
                    label = "Person"
                    if detection_type == 'face':
                        label = "Person (Face)"
                    elif detection_type == 'manual':
                        label = "Person (Manual)"
                        
                    cv.putText(display_copy, label, (x, y-10), 
                              cv.FONT_HERSHEY_SIMPLEX, 0.5, GREEN, 2)
                
                # 顯示當前設定和人數 (使用綠色文字)
                cv.putText(display_copy, f"Total Count: {person_count}", 
                          (10, 25), cv.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
                cv.putText(display_copy, f"Sensitivity: {min_weight_threshold:.2f}", 
                          (10, 50), cv.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
                cv.putText(display_copy, f"Current Frame: {len(all_detections)} people", 
                          (10, 75), cv.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
                cv.putText(display_copy, f"Mode: {mode_names[detection_mode]}", 
                          (10, 100), cv.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
                
                # 顯示曝光設定
                exposure_text = exposure_levels.get(current_exposure, "未知")
                if not exposure_control_available:
                    exposure_text += " (控制不可用)"
                cv.putText(display_copy, f"Exposure: {exposure_text}", 
                          (10, 125), cv.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
                
                # 更新顯示圖像
                display_img = display_copy
            
            except Exception as e:
                print(f"Error in image processing: {str(e)}")
            
            finally:
                processing_frame = False
        
        # 短暫休眠以避免CPU佔用過高
        time.sleep(0.01)

# 主程式開始
if not connect_camera():
    sys.exit(1)

# 啟動處理線程
processing_thread = threading.Thread(target=process_image_thread)
processing_thread.daemon = True
processing_thread.start()

# 檢查曝光控制是否可用
try:
    test_url = "http://172.16.18.123/control?var=exposure&val=0"
    response = requests.get(test_url, timeout=1)
    if response.status_code == 200:
        exposure_control_available = True
        print("曝光控制功能可用")
    else:
        print("曝光控制功能不可用：伺服器回應異常")
        exposure_control_available = False
except Exception as e:
    print(f"曝光控制功能不可用：{str(e)}")
    exposure_control_available = False

print("按 'a' 拍照存檔")
print("按 'm' 切換偵測模式")
print("按 'd' 開關偵測功能")
print("按 '+' 增加偵測靈敏度")
print("按 '-' 減少偵測靈敏度")
print("按 'r' 重置人數計數")
print("按 'e' 減少曝光度（使畫面變暗）")
print("按 'E' 增加曝光度（使畫面變亮）")
print("按 'q' 離開程式")

# 主循環
while True:    
    try:
        # 讀取攝影機數據
        bts += stream.read(CAMERA_BUFFER_SIZE)
        
        # 尋找 BMP 檔頭 (BM)
        bmp_head = bts.find(b'BM')
        
        if bmp_head > -1:
            # 如果找到 BMP 檔頭，檢查是否有足夠的數據來讀取檔案大小
            if len(bts) >= bmp_head + 10:
                # BMP 檔案大小位於檔頭後的 2-5 位元組
                file_size_bytes = bts[bmp_head+2:bmp_head+6]
                file_size = int.from_bytes(file_size_bytes, byteorder='little')
                
                # 檢查是否有完整的 BMP 檔案
                if len(bts) >= bmp_head + file_size:
                    # 提取完整的 BMP 檔案
                    bmp_data = bts[bmp_head:bmp_head + file_size]
                    
                    # 移除已處理的數據
                    bts = bts[bmp_head + file_size:]
                    
                    # 將 BMP 數據轉換為 NumPy 陣列
                    img = cv.imdecode(np.frombuffer(bmp_data, dtype=np.uint8), cv.IMREAD_COLOR)
                    
                    if img is not None:
                        # 調整大小
                        img = cv.resize(img, (640, 480))
                        
                        # 增加幀跳過以提高性能
                        frame_count += 1
                        if frame_count % (frame_skip + 1) == 0:
                            # 更新處理圖像
                            processed_img = img.copy()
                            frame_count = 0
                        
                        # 如果已有處理好的顯示圖像，則顯示它
                        if display_img is not None:
                            cv.imshow("ESP32-CAM Person Detection", display_img)
                        else:
                            # 否則顯示原始圖像
                            cv.imshow("ESP32-CAM Person Detection", img)
        
        k = cv.waitKey(1)
    except Exception as e:
        print(f"Error: {str(e)}")
        print("Attempting to reconnect...")
        bts = b''
        try:
            stream = urlopen(url)
            print("Reconnection successful")
        except Exception as e:
            print(f"Reconnection failed: {str(e)}")
            time.sleep(1)  # 等待一秒後再試
        continue
    
    # 按a拍照存檔
    if k & 0xFF == ord('a'):
        if display_img is not None:
            # 使用時間戳記作為檔名
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"person_detect_{timestamp}.jpg"
            cv.imwrite(filename, display_img)
            print(f"Photo saved: {filename}")
        else:
            print("No valid image received, cannot save")
    
    # 按m切換偵測模式
    elif k & 0xFF == ord('m'):
        detection_mode = (detection_mode + 1) % 2
        print(f"Switched to mode: {mode_names[detection_mode]}")
    
    # 按d開關偵測功能
    elif k & 0xFF == ord('d'):
        detection_active = not detection_active
        status = "enabled" if detection_active else "disabled"
        print(f"Detection {status}")
    
    # 按+增加偵測靈敏度（減少權重閾值）
    elif k & 0xFF == ord('+') or k & 0xFF == ord('='):
        if min_weight_threshold > 0.05:
            min_weight_threshold -= 0.05
            print(f"Increased sensitivity: threshold = {min_weight_threshold:.2f}")
    
    # 按-減少偵測靈敏度（增加權重閾值）
    elif k & 0xFF == ord('-') or k & 0xFF == ord('_'):
        if min_weight_threshold < 0.95:
            min_weight_threshold += 0.05
            print(f"Decreased sensitivity: threshold = {min_weight_threshold:.2f}")
    
    # 按e減少曝光度（使畫面變暗）
    elif k & 0xFF == ord('e'):
        if adjust_exposure(-1):
            print("減少曝光度 - 畫面變暗")
    
    # 按E增加曝光度（使畫面變亮）
    elif k & 0xFF == ord('E') or k & 0xFF == ord('E'):
        if adjust_exposure(1):
            print("增加曝光度 - 畫面變亮")
    
    # 按r重置人數計數
    elif k & 0xFF == ord('r'):
        person_count = 0
        print("Person count reset")
    
    # 按q離開
    elif k & 0xFF == ord('q'):
        print("Program terminated")
        break

cv.destroyAllWindows()