from ultralytics import YOLO
import cv2
import time

# 檢查 OpenCV 版本和 GUI 支援
print(f"OpenCV 版本: {cv2.__version__}")

try:
    # 設定視窗名稱及型態
    cv2.namedWindow('YOLOv8', cv2.WINDOW_NORMAL)
    has_gui = True
except cv2.error as e:
    print(f"GUI 支援錯誤: {e}")
    print("將以無 GUI 模式運行")
    has_gui = False

target = 0  # 使用攝像頭，或者改為 'city.mp4' 使用影片檔

# 載入 YOLO 模型
model = YOLO('yolov8n.pt')  # n,s,m,l,x 五種大小

# 顯示類別名稱
names = model.names
print(f"可檢測類別: {names}")

# 開啟攝像頭或影片
cap = cv2.VideoCapture(target)
if not cap.isOpened():
    print(f"無法開啟視訊來源: {target}")
    exit()

frame_count = 0
while True:
    st = time.time()  
    ret, frame = cap.read()
    if not ret:
        print("無法讀取影像，結束程序")
        break

    # 執行物件偵測
    results = model(frame, verbose=False)

    # 繪製偵測結果
    annotated_frame = results[0].plot()
    
    et = time.time()
    fps = round(1/(et-st), 1)  # 計算 FPS

    # 在畫面上顯示 FPS
    cv2.putText(annotated_frame, f'FPS: {fps}', (20, 50), 
                cv2.FONT_HERSHEY_PLAIN, 2, (0, 255, 255), 2)
    
    # 顯示偵測結果 (如果 GUI 可用)
    if has_gui:
        cv2.imshow('YOLOv8', annotated_frame)
        key = cv2.waitKey(1)
        if key == 27:  # ESC 鍵退出
            break
    
    # 每 30 幀顯示一次偵測資訊
    frame_count += 1
    if frame_count % 30 == 0:
        print(f"目前 FPS: {fps}")
        # 顯示偵測到的物體
        for r in results:
            for c in r.boxes.cls:
                class_id = int(c.item())
                print(f"偵測到: {names[class_id]}")

# 釋放資源
cap.release()
if has_gui:
    cv2.destroyAllWindows()

print("程序已結束")