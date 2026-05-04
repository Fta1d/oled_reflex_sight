import cv2

cap = cv2.VideoCapture(4)

if not cap.isOpened():
    print("Cannot open camera")
    exit()  

ret, frame = cap.read()
if not ret:
    print("Can't receive frame (stream end?). Exiting ...")
    exit()

h, w = frame.shape[:2]
cx, cy = w // 2, h // 2
color, thickness = (0, 255, 0), 1
cv2.line(frame, (cx - 20, cy), (cx + 20, cy), color, thickness)
cv2.line(frame, (cx, cy - 20), (cx, cy + 20), color, thickness)

cv2.namedWindow('frame', cv2.WINDOW_GUI_EXPANDED)
cv2.imshow('frame', frame)

cv2.waitKey(0)
cap.release()
cv2.destroyAllWindows()