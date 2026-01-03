// ===== 全域變數 =====
let mqttClient = null;
let deviceId = 'barleybox-001';
let isConnected = false;

// MQTT Topics
let topics = {
    telemetry: '',
    controlHeater: '',
    controlMist: '',
    controlFeed: '',
    controlMode: '',
    configIn: '',
    configOut: '',
    status: '',
    command: ''
};

// 圓盤儀表變數
let gauges = {
    tempEnv: { canvas: null, ctx: null },
    humEnv: { canvas: null, ctx: null },
    tempSub: { canvas: null, ctx: null }
};

// ===== 初始化 =====
document.addEventListener('DOMContentLoaded', function() {
    initializeTabs();
    initializeGauges();
    loadSavedSettings();
    
    // 連接按鈕事件
    document.getElementById('connectBtn').addEventListener('click', connectToDevice);
    
    // 初始化 Tooltip 點擊事件
    initializeTooltips();
    
    // 載入儲存的裝置設定
    const savedDeviceId = localStorage.getItem('deviceId');
    const savedBroker = localStorage.getItem('mqttBroker');
    
    if (savedDeviceId) {
        document.getElementById('deviceId').value = savedDeviceId;
    }
    if (savedBroker) {
        document.getElementById('mqttBroker').value = savedBroker;
    }
});

// ===== 初始化圓盤儀表 =====
function initializeGauges() {
    // 環境溫度儀表 (10-45°C) - 黃色漸層
    gauges.tempEnv.canvas = document.getElementById('tempEnvGauge');
    if (gauges.tempEnv.canvas) {
        gauges.tempEnv.ctx = gauges.tempEnv.canvas.getContext('2d');
        drawGauge(gauges.tempEnv, 25, 10, 45, 'tempEnv');
    }
    
    // 環境濕度儀表 (0-100%) - 藍色漸層
    gauges.humEnv.canvas = document.getElementById('humEnvGauge');
    if (gauges.humEnv.canvas) {
        gauges.humEnv.ctx = gauges.humEnv.canvas.getContext('2d');
        drawGauge(gauges.humEnv, 50, 0, 100, 'humEnv');
    }
    
    // 基質溫度儀表 (10-45°C) - 紅色漸層
    gauges.tempSub.canvas = document.getElementById('tempSubGauge');
    if (gauges.tempSub.canvas) {
        gauges.tempSub.ctx = gauges.tempSub.canvas.getContext('2d');
        drawGauge(gauges.tempSub, 25, 10, 45, 'tempSub');
    }
}

// ===== 繪製圓盤儀表 =====
function drawGauge(gauge, value, minValue, maxValue, type) {
    if (!gauge.canvas || !gauge.ctx) return;
    
    const canvas = gauge.canvas;
    const ctx = gauge.ctx;
    const centerX = canvas.width / 2;
    const centerY = canvas.height / 2;
    const radius = Math.min(centerX, centerY) - 20;
    
    // 清空畫布
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // 創建漸層色彩
    let gradient;
    if (type === 'tempEnv') {
        // 環境溫度：淺黃色 -> 深黃色
        gradient = ctx.createLinearGradient(centerX - radius, centerY, centerX + radius, centerY);
        gradient.addColorStop(0, '#FFF3CD');    // 淺黃色
        gradient.addColorStop(0.5, '#FFD60A');  // 中黃色
        gradient.addColorStop(1, '#FF9500');    // 深黃色
    } else if (type === 'humEnv') {
        // 環境濕度：淺藍色 -> 深藍色
        gradient = ctx.createLinearGradient(centerX - radius, centerY, centerX + radius, centerY);
        gradient.addColorStop(0, '#E3F2FD');    // 淺藍色
        gradient.addColorStop(0.5, '#64B5F6');  // 中藍色
        gradient.addColorStop(1, '#1976D2');    // 深藍色
    } else if (type === 'tempSub') {
        // 基質溫度：淺紅色 -> 深紅色
        gradient = ctx.createLinearGradient(centerX - radius, centerY, centerX + radius, centerY);
        gradient.addColorStop(0, '#FFEBEE');    // 淺紅色
        gradient.addColorStop(0.5, '#EF5350');  // 中紅色
        gradient.addColorStop(1, '#C62828');    // 深紅色
    }
    
    // 繪製背景圓環
    ctx.beginPath();
    ctx.arc(centerX, centerY, radius, 0.75 * Math.PI, 2.25 * Math.PI);
    ctx.lineWidth = 14;
    ctx.strokeStyle = 'rgba(0, 0, 0, 0.1)';
    ctx.stroke();
    
    // 計算進度（按比例）
    const range = maxValue - minValue;
    const normalizedValue = Math.min(Math.max(value - minValue, 0), range);
    const percentage = normalizedValue / range;
    const endAngle = 0.75 * Math.PI + (1.5 * Math.PI * percentage);
    
    // 繪製進度圓環（漸層）
    ctx.beginPath();
    ctx.arc(centerX, centerY, radius, 0.75 * Math.PI, endAngle);
    ctx.lineWidth = 14;
    ctx.strokeStyle = gradient;
    ctx.lineCap = 'round';
    ctx.stroke();
    
    // 繪製刻度和數值標籤
    const scaleCount = 6; // 6個主要刻度點
    for (let i = 0; i <= scaleCount; i++) {
        const angle = 0.75 * Math.PI + (1.5 * Math.PI * i / scaleCount);
        const scaleValue = minValue + (range * i / scaleCount);
        
        // 主刻度線
        const startRadius = radius - 10;
        const endRadius = radius + 5;
        
        const x1 = centerX + startRadius * Math.cos(angle);
        const y1 = centerY + startRadius * Math.sin(angle);
        const x2 = centerX + endRadius * Math.cos(angle);
        const y2 = centerY + endRadius * Math.sin(angle);
        
        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.lineWidth = 3;
        ctx.strokeStyle = 'rgba(0, 0, 0, 0.4)';
        ctx.stroke();
        
        // 數值標籤（只顯示最小值和最大值）
        if (i === 0 || i === scaleCount) {
            const labelRadius = radius + 15;
            const labelX = centerX + labelRadius * Math.cos(angle);
            const labelY = centerY + labelRadius * Math.sin(angle);
            
            ctx.font = '10px -apple-system, BlinkMacSystemFont, sans-serif';
            ctx.fillStyle = 'rgba(0, 0, 0, 0.6)';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            
            const displayValue = type === 'humidity' ? 
                `${Math.round(scaleValue)}%` : 
                `${Math.round(scaleValue)}°`;
            ctx.fillText(displayValue, labelX, labelY);
        }
    }
    
    // 繪製小刻度線
    for (let i = 0; i <= scaleCount * 2; i++) {
        if (i % 2 !== 0) { // 只繪製中間的小刻度
            const angle = 0.75 * Math.PI + (1.5 * Math.PI * i / (scaleCount * 2));
            const startRadius = radius - 6;
            const endRadius = radius + 2;
            
            const x1 = centerX + startRadius * Math.cos(angle);
            const y1 = centerY + startRadius * Math.sin(angle);
            const x2 = centerX + endRadius * Math.cos(angle);
            const y2 = centerY + endRadius * Math.sin(angle);
            
            ctx.beginPath();
            ctx.moveTo(x1, y1);
            ctx.lineTo(x2, y2);
            ctx.lineWidth = 1;
            ctx.strokeStyle = 'rgba(0, 0, 0, 0.2)';
            ctx.stroke();
        }
    }
}

// ===== 更新圓盤儀表 =====
function updateGauge(type, value) {
    let minValue, maxValue;
    
    switch(type) {
        case 'tempEnv':
            minValue = 10;
            maxValue = 45;
            break;
        case 'humEnv':
            minValue = 0;
            maxValue = 100;
            break;
        case 'tempSub':
            minValue = 10;
            maxValue = 45;
            break;
    }
    
    drawGauge(gauges[type], value, minValue, maxValue, type);
}

// ===== 標籤頁切換 =====
function initializeTabs() {
    const tabButtons = document.querySelectorAll('.tab-btn, .tab-btn-compact');
    
    tabButtons.forEach(button => {
        button.addEventListener('click', function() {
            // 移除所有 active 類別
            tabButtons.forEach(btn => btn.classList.remove('active'));
            document.querySelectorAll('.config-panel, .config-panel-compact').forEach(panel => panel.classList.remove('active'));
            
            // 添加 active 到當前標籤
            this.classList.add('active');
            const tabName = this.getAttribute('data-tab');
            document.getElementById(tabName + '-panel').classList.add('active');
        });
    });
}

// ===== 計算 Tooltip 位置 =====
function calculateTooltipPosition(icon) {
    const tooltipText = icon.getAttribute('data-tooltip');
    if (!tooltipText) return;
    
    const isRightAligned = icon.classList.contains('param-help-right');
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;
    const margin = 20; // 邊界安全距離
    
    // 創建臨時元素來測量 tooltip 實際尺寸
    const tempDiv = document.createElement('div');
    tempDiv.style.cssText = 'position: absolute; visibility: hidden; white-space: normal; max-width: 320px; min-width: 200px; padding: 12px 16px; font-size: 13px; line-height: 1.5; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft JhengHei", sans-serif; word-wrap: break-word; box-sizing: border-box;';
    tempDiv.textContent = tooltipText;
    document.body.appendChild(tempDiv);
    
    const tooltipWidth = tempDiv.offsetWidth;
    const tooltipHeight = tempDiv.offsetHeight;
    document.body.removeChild(tempDiv);
    
    if (isRightAligned) {
        // 對於 param-help-right，計算是否會超出右邊界
        const iconRect = icon.getBoundingClientRect();
        const spaceRight = viewportWidth - iconRect.right - margin - 12; // 12px 是 margin-left
        const spaceLeft = iconRect.left - margin;
        
        // 如果右側空間不足（會導致超出右邊界），或者左側空間不足（會導致超出左邊界），改用固定右側顯示
        if (spaceRight < tooltipWidth || (iconRect.right + 12 + tooltipWidth) > (viewportWidth - margin)) {
            icon.classList.add('tooltip-fallback-right');
        } else {
            icon.classList.remove('tooltip-fallback-right');
        }
    } else {
        // 對於一般 tooltip，max-width 已經確保至少距離左右邊界 20px
        // 不需要額外處理
    }
}

// ===== 初始化 Tooltip 點擊事件 =====
function initializeTooltips() {
    const helpIcons = document.querySelectorAll('.param-help');
    
    helpIcons.forEach(icon => {
        // 點擊切換顯示/隱藏
        icon.addEventListener('click', function(e) {
            e.stopPropagation();
            
            // 關閉其他所有 tooltip
            helpIcons.forEach(other => {
                if (other !== this) {
                    other.classList.remove('active');
                }
            });
            
            // 計算位置後顯示
            calculateTooltipPosition(this);
            this.classList.toggle('active');
        });
        
        // 懸停顯示
        icon.addEventListener('mouseenter', function() {
            // 關閉其他所有 tooltip
            helpIcons.forEach(other => {
                if (other !== this) {
                    other.classList.remove('active');
                }
            });
            
            // 計算位置後顯示
            calculateTooltipPosition(this);
            this.classList.add('active');
        });
        
        // 滑鼠離開隱藏（僅針對懸停觸發的）
        icon.addEventListener('mouseleave', function() {
            // 延遲隱藏，給用戶時間移動到 tooltip 上
            setTimeout(() => {
                if (!this.matches(':hover')) {
                    this.classList.remove('active');
                }
            }, 100);
        });
    });
    
    // 視窗大小改變時重新計算位置
    let resizeTimer;
    window.addEventListener('resize', function() {
        clearTimeout(resizeTimer);
        resizeTimer = setTimeout(function() {
            helpIcons.forEach(icon => {
                if (icon.classList.contains('active')) {
                    calculateTooltipPosition(icon);
                }
            });
        }, 100);
    });
    
    // 點擊頁面其他地方關閉所有 tooltip
    document.addEventListener('click', function(e) {
        // 如果點擊的不是問號圖示，則關閉所有 tooltip
        if (!e.target.classList.contains('param-help')) {
            helpIcons.forEach(icon => {
                icon.classList.remove('active');
            });
        }
    });
    
    // ESC 鍵關閉 tooltip
    document.addEventListener('keydown', function(e) {
        if (e.key === 'Escape') {
            helpIcons.forEach(icon => {
                icon.classList.remove('active');
            });
        }
    });
}

// ===== 設備控制切換 =====
function toggleControl(device) {
    const menu = document.getElementById(device + 'Menu');
    
    // 關閉其他菜單
    document.querySelectorAll('.control-menu').forEach(m => {
        if (m.id !== device + 'Menu') {
            m.classList.remove('active');
        }
    });
    
    // 切換當前菜單
    menu.classList.toggle('active');
}

// ===== MQTT 連接 =====
function connectToDevice() {
    const deviceIdInput = document.getElementById('deviceId').value.trim();
    const brokerInput = document.getElementById('mqttBroker').value.trim();
    
    if (!deviceIdInput || !brokerInput) {
        addLog('請輸入裝置 ID 和 MQTT Broker 地址', 'error');
        return;
    }
    
    deviceId = deviceIdInput;
    
    // 儲存設定
    localStorage.setItem('deviceId', deviceId);
    localStorage.setItem('mqttBroker', brokerInput);
    
    // 初始化 topics
    topics.telemetry = `farm/telemetry/${deviceId}`;
    topics.controlHeater = `farm/control/${deviceId}/heater`;
    topics.controlMist = `farm/control/${deviceId}/mist`;
    topics.controlFeed = `farm/control/${deviceId}/feed`;
    topics.controlMode = `farm/control/${deviceId}/mode`;
    topics.configIn = `farm/config/${deviceId}`;
    topics.configOut = `farm/config/${deviceId}/current`;
    topics.status = `farm/status/${deviceId}`;
    topics.command = `farm/command/${deviceId}`;
    
    addLog(`正在連接到 ${brokerInput}...`, 'info');
    
    // 使用 WebSocket 連接
    const wsUrl = `wss://${brokerInput}:8084/mqtt`;
    
    try {
        mqttClient = mqtt.connect(wsUrl, {
            clientId: `web-${deviceId}-${Math.random().toString(16).substr(2, 8)}`,
            clean: true,
            reconnectPeriod: 5000
        });
        
        mqttClient.on('connect', onMqttConnect);
        mqttClient.on('message', onMqttMessage);
        mqttClient.on('error', onMqttError);
        mqttClient.on('offline', onMqttOffline);
        mqttClient.on('reconnect', onMqttReconnect);
        
    } catch (error) {
        addLog(`連接失敗: ${error.message}`, 'error');
        updateConnectionStatus(false);
    }
}

function onMqttConnect() {
    isConnected = true;
    updateConnectionStatus(true);
    addLog('成功連接到 MQTT Broker', 'success');
    
    // 訂閱所有相關 topics
    mqttClient.subscribe(topics.telemetry, {qos: 0});
    mqttClient.subscribe(topics.configOut, {qos: 0});
    mqttClient.subscribe(topics.status, {qos: 0});
    
    addLog(`已訂閱裝置 ${deviceId} 的資料`, 'success');
    
    // 延遲請求配置
    setTimeout(() => {
        loadCurrentConfig();
    }, 2000);
}

function onMqttMessage(topic, message) {
    try {
        const payload = JSON.parse(message.toString());
        
        if (topic === topics.telemetry) {
            updateTelemetry(payload);
        } else if (topic === topics.configOut) {
            updateConfigDisplay(payload);
        } else if (topic === topics.status) {
            handleStatusUpdate(payload);
        }
        
        updateLastUpdateTime();
    } catch (error) {
        console.error('解析 MQTT 訊息失敗:', error);
        addLog(`訊息解析錯誤: ${error.message}`, 'error');
    }
}

function onMqttError(error) {
    addLog(`MQTT 錯誤: ${error.message}`, 'error');
    updateConnectionStatus(false);
}

function onMqttOffline() {
    isConnected = false;
    updateConnectionStatus(false);
    addLog('與 MQTT Broker 連接中斷', 'warning');
}

function onMqttReconnect() {
    addLog('正在重新連接...', 'info');
}

// ===== 更新連接狀態 =====
function updateConnectionStatus(connected) {
    const statusDot = document.querySelector('.status-dot');
    const statusText = document.querySelector('.status-text');
    
    if (connected) {
        statusDot.classList.add('connected');
        statusDot.classList.remove('disconnected');
        statusText.textContent = '已連接';
    } else {
        statusDot.classList.remove('connected');
        statusDot.classList.add('disconnected');
        statusText.textContent = '未連接';
    }
}

// ===== 更新遙測數據 =====
function updateTelemetry(data) {
    // 環境溫度
    if (data.temp_env !== undefined && !isNaN(data.temp_env)) {
        const tempEnvValue = data.temp_env;
        document.getElementById('tempEnv').textContent = tempEnvValue.toFixed(1);
        updateGauge('tempEnv', tempEnvValue);
    } else {
        document.getElementById('tempEnv').textContent = '--';
        updateGauge('tempEnv', 10); // 顯示最小值
    }
    
    // 環境濕度
    if (data.hum_env !== undefined && !isNaN(data.hum_env)) {
        const humEnvValue = data.hum_env;
        document.getElementById('humEnv').textContent = humEnvValue.toFixed(1);
        updateGauge('humEnv', humEnvValue);
    } else {
        document.getElementById('humEnv').textContent = '--';
        updateGauge('humEnv', 0); // 顯示最小值
    }
    
    // 基質溫度
    if (data.temp_sub !== undefined && data.temp_sub !== null && !isNaN(data.temp_sub)) {
        const tempSubValue = data.temp_sub;
        document.getElementById('tempSub').textContent = tempSubValue.toFixed(1);
        updateGauge('tempSub', tempSubValue);
    } else {
        document.getElementById('tempSub').textContent = '--';
        updateGauge('tempSub', 10); // 顯示最小值
    }
    
    // 運作模式
    if (data.mode) {
        // 更新運作模式控制顯示
        const modeBadge = document.getElementById('modeBadge');
        const modeLogo = document.getElementById('modeLogo');
        
        if (data.mode === 'AUTO') {
            modeBadge.textContent = 'AUTO';
            modeBadge.classList.add('auto');
            modeBadge.classList.remove('manual');
            modeLogo.classList.add('active');
        } else {
            modeBadge.textContent = 'MANUAL';
            modeBadge.classList.add('manual');
            modeBadge.classList.remove('auto');
            modeLogo.classList.remove('active');
        }
    }
    
    // 加熱器狀態
    if (data.heater_on !== undefined) {
        updateDeviceStatus('heater', data.heater_on);
    }
    
    // 噴霧器狀態
    if (data.mist_on !== undefined) {
        updateDeviceStatus('mist', data.mist_on);
    }
}

// ===== 更新設備狀態 =====
function updateDeviceStatus(device, isOn) {
    const badge = document.getElementById(`${device}Badge`);
    const logo = document.getElementById(`${device}Logo`);
    
    if (isOn) {
        badge.textContent = 'ON';
        badge.classList.add('on');
        badge.classList.remove('auto');
        logo.classList.add('active');
    } else {
        badge.textContent = 'OFF';
        badge.classList.remove('on', 'auto');
        logo.classList.remove('active');
    }
}

// ===== 處理狀態更新 =====
function handleStatusUpdate(data) {
    if (data.event === 'feed') {
        addLog('餵食器已執行餵食動作', 'success');
        const feedBadge = document.getElementById('feedBadge');
        feedBadge.textContent = '已餵食';
        setTimeout(() => {
            feedBadge.textContent = '待命';
        }, 3000);
    }
    
    if (data.warning) {
        addLog(`警告: ${data.warning}`, 'warning');
    }
    
    if (data.status === 'online') {
        addLog('裝置已上線', 'success');
    }
}

// ===== 控制函數 =====
function controlHeater(action) {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlHeater, action);
    addLog(`已發送加熱器控制指令: ${action}`, 'info');
    
    // 更新狀態顯示
    const badge = document.getElementById('heaterBadge');
    const logo = document.getElementById('heaterLogo');
    
    if (action === 'ON') {
        badge.textContent = 'ON';
        badge.classList.add('on');
        badge.classList.remove('auto');
        logo.classList.add('active');
    } else if (action === 'OFF') {
        badge.textContent = 'OFF';
        badge.classList.remove('on', 'auto');
        logo.classList.remove('active');
    } else if (action === 'AUTO') {
        badge.textContent = 'AUTO';
        badge.classList.add('auto');
        badge.classList.remove('on');
        logo.classList.remove('active');
    }
    
    // 關閉菜單
    document.getElementById('heaterMenu').classList.remove('active');
}

function controlMist(action) {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlMist, action);
    addLog(`已發送噴霧器控制指令: ${action}`, 'info');
    
    // 更新狀態顯示
    const badge = document.getElementById('mistBadge');
    const logo = document.getElementById('mistLogo');
    
    if (action === 'ON') {
        badge.textContent = 'ON';
        badge.classList.add('on');
        badge.classList.remove('auto');
        logo.classList.add('active');
    } else if (action === 'OFF') {
        badge.textContent = 'OFF';
        badge.classList.remove('on', 'auto');
        logo.classList.remove('active');
    } else if (action === 'AUTO') {
        badge.textContent = 'AUTO';
        badge.classList.add('auto');
        badge.classList.remove('on');
        logo.classList.remove('active');
    }
    
    // 關閉菜單
    document.getElementById('mistMenu').classList.remove('active');
}

function triggerFeed() {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlFeed, 'TRIGGER');
    addLog('已觸發餵食動作', 'info');
    
    // 更新狀態顯示
    const badge = document.getElementById('feedBadge');
    badge.textContent = '餵食中';
    setTimeout(() => {
        badge.textContent = '待命';
    }, 3000);
}

function controlMode(action) {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlMode, action);
    addLog(`已切換運作模式: ${action}`, 'info');
    
    // 更新狀態顯示
    const badge = document.getElementById('modeBadge');
    const logo = document.getElementById('modeLogo');
    
    if (action === 'AUTO') {
        badge.textContent = 'AUTO';
        badge.classList.add('auto');
        badge.classList.remove('manual');
        logo.classList.add('active');
    } else if (action === 'MANUAL') {
        badge.textContent = 'MANUAL';
        badge.classList.add('manual');
        badge.classList.remove('auto');
        logo.classList.remove('active');
    }
    
    // 關閉菜單
    document.getElementById('modeMenu').classList.remove('active');
}

// ===== 讀取目前設定 =====
function loadCurrentConfig() {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.command, 'publish_config');
    addLog('正在讀取裝置設定...', 'info');
}

// ===== 更新設定顯示 =====
function updateConfigDisplay(config) {
    // 溫度控制
    if (config.T_heat_on !== undefined) {
        document.getElementById('tHeatOn').value = config.T_heat_on;
    }
    if (config.T_heat_off !== undefined) {
        document.getElementById('tHeatOff').value = config.T_heat_off;
    }
    if (config.heater_max_temp !== undefined) {
        document.getElementById('heaterMaxTemp').value = config.heater_max_temp;
    }
    if (config.ntc_low_temp_threshold !== undefined) {
        document.getElementById('ntcLowTemp').value = config.ntc_low_temp_threshold;
    }
    if (config.ntc_heat_on_minutes !== undefined) {
        document.getElementById('ntcHeatMinutes').value = config.ntc_heat_on_minutes;
    }
    if (config.ntc_adc_vref !== undefined) {
        document.getElementById('ntcAdcVref').value = config.ntc_adc_vref;
    }
    if (config.ntc_temp_offset !== undefined) {
        document.getElementById('ntcTempOffset').value = config.ntc_temp_offset;
    }
    
    // 濕度控制
    if (config.H_mist_on !== undefined) {
        document.getElementById('hMistOn').value = config.H_mist_on;
    }
    if (config.H_mist_off !== undefined) {
        document.getElementById('hMistOff').value = config.H_mist_off;
    }
    if (config.mist_max_on_seconds !== undefined) {
        document.getElementById('mistMaxOn').value = config.mist_max_on_seconds;
    }
    if (config.mist_min_off_seconds !== undefined) {
        document.getElementById('mistMinOff').value = config.mist_min_off_seconds;
    }
    
    // 餵食設定
    if (config.feed_duration_ms !== undefined) {
        document.getElementById('feedDuration').value = (config.feed_duration_ms / 1000).toFixed(1); // 毫秒轉秒
    }
    if (config.feed_min_interval_hours !== undefined) {
        document.getElementById('feedMinIntervalHours').value = config.feed_min_interval_hours;
    }
    if (config.feed_times_csv !== undefined) {
        document.getElementById('feedTimes').value = config.feed_times_csv;
    }
    
    // 系統設定
    if (config.upload_interval_seconds !== undefined) {
        document.getElementById('uploadInterval').value = Math.round(config.upload_interval_seconds / 60);
    }
    
    addLog('已載入裝置設定', 'success');
}

// ===== 儲存設定 =====
function saveConfig() {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    const config = {};
    
    // 溫度控制
    const tHeatOn = parseFloat(document.getElementById('tHeatOn').value);
    const tHeatOff = parseFloat(document.getElementById('tHeatOff').value);
    const heaterMaxTemp = parseFloat(document.getElementById('heaterMaxTemp').value);
    const ntcLowTemp = parseFloat(document.getElementById('ntcLowTemp').value);
    const ntcHeatMinutes = parseInt(document.getElementById('ntcHeatMinutes').value);
    const ntcAdcVref = parseFloat(document.getElementById('ntcAdcVref').value);
    const ntcTempOffset = parseFloat(document.getElementById('ntcTempOffset').value);
    
    if (!isNaN(tHeatOn)) config.T_heat_on = tHeatOn;
    if (!isNaN(tHeatOff)) config.T_heat_off = tHeatOff;
    if (!isNaN(heaterMaxTemp)) config.heater_max_temp = heaterMaxTemp;
    if (!isNaN(ntcLowTemp)) config.ntc_low_temp_threshold = ntcLowTemp;
    if (!isNaN(ntcHeatMinutes)) config.ntc_heat_on_minutes = ntcHeatMinutes;
    if (!isNaN(ntcAdcVref)) config.ntc_adc_vref = ntcAdcVref;
    if (!isNaN(ntcTempOffset)) config.ntc_temp_offset = ntcTempOffset;
    
    // 濕度控制
    const hMistOn = parseFloat(document.getElementById('hMistOn').value);
    const hMistOff = parseFloat(document.getElementById('hMistOff').value);
    const mistMaxOn = parseInt(document.getElementById('mistMaxOn').value);
    const mistMinOff = parseInt(document.getElementById('mistMinOff').value);
    
    if (!isNaN(hMistOn)) config.H_mist_on = hMistOn;
    if (!isNaN(hMistOff)) config.H_mist_off = hMistOff;
    if (!isNaN(mistMaxOn)) config.mist_max_on_seconds = mistMaxOn;
    if (!isNaN(mistMinOff)) config.mist_min_off_seconds = mistMinOff;
    
    // 餵食設定
    const feedDurationSeconds = parseFloat(document.getElementById('feedDuration').value);
    const feedMinIntervalHours = parseInt(document.getElementById('feedMinIntervalHours').value);
    const feedTimes = document.getElementById('feedTimes').value;
    
    if (!isNaN(feedDurationSeconds)) config.feed_duration_ms = Math.round(feedDurationSeconds * 1000); // 秒轉毫秒
    if (!isNaN(feedMinIntervalHours) && feedMinIntervalHours >= 1 && feedMinIntervalHours <= 24) {
        config.feed_min_interval_hours = feedMinIntervalHours;
    }
    if (feedTimes) config.feed_times_csv = feedTimes;
    
    // 系統設定
    const uploadIntervalMinutes = parseInt(document.getElementById('uploadInterval').value);
    
    if (!isNaN(uploadIntervalMinutes)) config.upload_interval_seconds = uploadIntervalMinutes * 60;
    
    // 發送設定
    const payload = JSON.stringify(config);
    mqttClient.publish(topics.configIn, payload);
    
    addLog('已發送設定到裝置', 'success');
    
    // 儲存到本地
    localStorage.setItem(`config_${deviceId}`, payload);
}

// ===== 載入儲存的設定 =====
function loadSavedSettings() {
    const savedDeviceId = localStorage.getItem('deviceId');
    if (!savedDeviceId) return;
    
    const savedConfig = localStorage.getItem(`config_${savedDeviceId}`);
    if (savedConfig) {
        try {
            const config = JSON.parse(savedConfig);
            updateConfigDisplay(config);
        } catch (error) {
            console.error('載入儲存的設定失敗:', error);
        }
    }
}

// ===== 日誌系統 =====
function addLog(message, type = 'info') {
    const logContainer = document.getElementById('logContainer');
    const logEntry = document.createElement('div');
    logEntry.className = `log-entry-compact log-${type}`;
    
    const now = new Date();
    const timeString = now.toLocaleTimeString('zh-TW', { hour12: false });
    
    logEntry.innerHTML = `
        <span class="log-time-compact">${timeString}</span>
        <span class="log-message-compact">${message}</span>
    `;
    
    // 插入到最前面
    if (logContainer.firstChild) {
        logContainer.insertBefore(logEntry, logContainer.firstChild);
    } else {
        logContainer.appendChild(logEntry);
    }
    
    // 限制日誌數量
    const maxLogs = 30;
    while (logContainer.children.length > maxLogs) {
        logContainer.removeChild(logContainer.lastChild);
    }
}

// ===== 更新最後更新時間 =====
function updateLastUpdateTime() {
    const now = new Date();
    const timeString = now.toLocaleString('zh-TW', { 
        year: 'numeric',
        month: '2-digit',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
        hour12: false
    });
    const lastUpdateElement = document.getElementById('lastUpdate');
    if (lastUpdateElement) {
        lastUpdateElement.textContent = timeString;
    }
}

// ===== 定期檢查連接狀態 =====
setInterval(() => {
    if (mqttClient && !mqttClient.connected) {
        updateConnectionStatus(false);
    }
}, 5000);

// ===== 視窗大小改變時重繪儀表盤 =====
window.addEventListener('resize', () => {
    // 重繪所有儀表盤
    setTimeout(() => {
        const tempEnvValue = parseFloat(document.getElementById('tempEnv').textContent);
        const humEnvValue = parseFloat(document.getElementById('humEnv').textContent);
        const tempSubValue = parseFloat(document.getElementById('tempSub').textContent);
        
        if (!isNaN(tempEnvValue)) {
            updateGauge('tempEnv', tempEnvValue, 50);
        }
        if (!isNaN(humEnvValue)) {
            updateGauge('humEnv', humEnvValue, 100);
        }
        if (!isNaN(tempSubValue)) {
            updateGauge('tempSub', tempSubValue, 50);
    }
    }, 100);
});