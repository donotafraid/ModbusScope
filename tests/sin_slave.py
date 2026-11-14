import math
import time
import signal
import socketserver
from pymodbus.server.sync import ModbusTcpServer
from pymodbus.datastore import ModbusSlaveContext, ModbusServerContext, ModbusSequentialDataBlock

# ... (前面的 SineWaveSlaveContext 和 toggle_pause 代码保持不变) ...
# 全局状态控制
is_paused = False
pause_offset = 0.0

def toggle_pause(signum, frame):
    global is_paused, pause_offset
    if not is_paused:
        is_paused = True
        pause_offset = time.time()
        print("\n[模拟器] ⏸️  已暂停生成数据 (数据保持冻结)。再按一次 Ctrl+Z 恢复...")
    else:
        is_paused = False
        print("\n[模拟器] ▶️  已恢复生成数据...")

class SineWaveSlaveContext(ModbusSlaveContext):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._start_time = time.time()

    def getValues(self, fx, address, count=1):
        global is_paused, pause_offset
        if fx == 3:
            values = []
            now = pause_offset if is_paused else time.time()
            for i in range(count):
                temp = 25.0 + 5.0 * math.sin(2 * math.pi * now / 10.0)
                values.append(int(temp * 10) & 0xFFFF)
            return values
        return super().getValues(fx, address, count)


# ================= 核心改动在这里 =================
class ReusableModbusTcpServer(ModbusTcpServer):
    """继承并重写 bind 逻辑，强制开启 SO_REUSEADDR"""
    def server_bind(self):
        self.socket.setsockopt(socketserver.socket.SOL_SOCKET, socketserver.socket.SO_REUSEADDR, 1)
        super().server_bind()  # 调用父类真正的 bind

def run_server():
    signal.signal(signal.SIGTSTP, toggle_pause)
    
    block = ModbusSequentialDataBlock(0, [0] * 100)
    slave = SineWaveSlaveContext(hr=block)
    context = ModbusServerContext(slaves={1: slave}, single=False)
    
    print("Modbus TCP 正弦波模拟从站已启动，监听 0.0.0.0:5020")
    print("提示：按 Ctrl+Z 暂停/恢复，按 Ctrl+C 彻底关闭")
    
    # 使用我们重写了 bind 逻辑的类
    server = ReusableModbusTcpServer(context, address=("0.0.0.0", 5020))
    server.serve_forever()
# =================================================

if __name__ == "__main__":
    run_server()
