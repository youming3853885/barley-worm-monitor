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
    configIn: '',
    configOut: '',
    status: '',
    command: ''
};

// ===== 初始化 =====
document.addEventListener('DOMContentLoaded', function() {
    initializeTabs();
    loadSavedSettings();
    
    // 連接按鈕事件
    document.getElementById('connectBtn').addEventListener('click', connectToDevice);
    
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
    topics.configIn = `farm/config/${deviceId}`;
    topics.configOut = `farm/config/${deviceId}/current`;
    topics.status = `farm/status/${deviceId}`;
    topics.command = `farm/command/${deviceId}`;
    
    addLog(`正在連接到 ${brokerInput}...`, 'info');
    
    // 使用 WebSocket 連接
    // broker.MQTTGO.io: WSS port 8084, MQTT port 1883
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
    
    // 訂閱所有相關 topics (QoS 0)
    mqttClient.subscribe(topics.telemetry, {qos: 0}, (err) => {
        if (!err) {
            console.log('✅ 已訂閱:', topics.telemetry);
        } else {
            console.error('❌ 訂閱失敗:', topics.telemetry, err);
        }
    });
    mqttClient.subscribe(topics.configOut, {qos: 0}, (err) => {
        if (!err) {
            console.log('✅ 已訂閱:', topics.configOut);
        } else {
            console.error('❌ 訂閱失敗:', topics.configOut, err);
        }
    });
    mqttClient.subscribe(topics.status, {qos: 0}, (err) => {
        if (!err) {
            console.log('✅ 已訂閱:', topics.status);
        } else {
            console.error('❌ 訂閱失敗:', topics.status, err);
        }
    });
    
    addLog(`已訂閱裝置 ${deviceId} 的資料`, 'success');
    addLog('等待 Arduino 裝置上線...', 'info');
    
    // 延遲請求配置，給 Arduino 時間連接
    setTimeout(() => {
        loadCurrentConfig();
    }, 2000);
}

function onMqttMessage(topic, message) {
    try {
        const payload = JSON.parse(message.toString());
        
        // 除錯：顯示收到的訊息
        console.log('📨 收到 MQTT 訊息 [' + new Date().toLocaleTimeString() + ']:', topic, payload);
        
        if (topic === topics.telemetry) {
            updateTelemetry(payload);
            addLog('收到遙測資料', 'success');
        } else if (topic === topics.configOut) {
            updateConfigDisplay(payload);
        } else if (topic === topics.status) {
            handleStatusUpdate(payload);
        } else {
            console.warn('⚠️ 收到未處理的 Topic:', topic);
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
    const tempEnvEl = document.getElementById('tempEnv');
    if (data.temp_env !== undefined && !isNaN(data.temp_env)) {
        tempEnvEl.innerHTML = data.temp_env.toFixed(1) + '<span>°C</span>';
    } else {
        tempEnvEl.innerHTML = '--<span>°C</span>';
    }
    
    // 環境濕度
    const humEnvEl = document.getElementById('humEnv');
    if (data.hum_env !== undefined && !isNaN(data.hum_env)) {
        humEnvEl.innerHTML = data.hum_env.toFixed(1) + '<span>%</span>';
    } else {
        humEnvEl.innerHTML = '--<span>%</span>';
    }
    
    // 基質溫度
    const tempSubEl = document.getElementById('tempSub');
    if (data.temp_sub !== undefined && data.temp_sub !== null && !isNaN(data.temp_sub)) {
        tempSubEl.innerHTML = data.temp_sub.toFixed(1) + '<span>°C</span>';
    } else {
        tempSubEl.innerHTML = '--<span>°C</span>';
    }
    
    // 運作模式
    if (data.mode) {
        const modeElement = document.getElementById('mode');
        modeElement.textContent = data.mode === 'AUTO' ? '自動' : '手動';
        modeElement.style.color = data.mode === 'AUTO' ? 'var(--color-success)' : 'var(--color-warning)';
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
    const statusElement = document.getElementById(`${device}Status`);
    
    if (isOn) {
        statusElement.textContent = '運作中';
        statusElement.classList.add('active');
        statusElement.classList.remove('inactive');
    } else {
        statusElement.textContent = '關閉';
        statusElement.classList.remove('active');
        statusElement.classList.add('inactive');
    }
}

// ===== 處理狀態更新 =====
function handleStatusUpdate(data) {
    if (data.event === 'feed') {
        addLog('餵食器已執行餵食動作', 'success');
        document.getElementById('feedStatus').textContent = '已餵食';
        setTimeout(() => {
            document.getElementById('feedStatus').textContent = '待命';
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
}

function controlMist(action) {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlMist, action);
    addLog(`已發送噴霧器控制指令: ${action}`, 'info');
}

function triggerFeed() {
    if (!isConnected) {
        addLog('請先連接裝置', 'error');
        return;
    }
    
    mqttClient.publish(topics.controlFeed, 'TRIGGER');
    addLog('已觸發餵食動作', 'info');
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
    if (config.feed_interval_seconds !== undefined) {
        // 從秒轉換為分鐘顯示
        document.getElementById('feedInterval').value = Math.round(config.feed_interval_seconds / 60);
    }
    if (config.feed_duration_ms !== undefined) {
        document.getElementById('feedDuration').value = config.feed_duration_ms;
    }
    if (config.feed_times_csv !== undefined) {
        document.getElementById('feedTimes').value = config.feed_times_csv;
    }
    
    // 系統設定
    if (config.upload_interval_seconds !== undefined) {
        // 從秒轉換為分鐘顯示
        document.getElementById('uploadInterval').value = Math.round(config.upload_interval_seconds / 60);
    }
    if (config.mode !== undefined) {
        document.getElementById('modeSelect').value = config.mode;
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
    
    if (!isNaN(tHeatOn)) config.T_heat_on = tHeatOn;
    if (!isNaN(tHeatOff)) config.T_heat_off = tHeatOff;
    if (!isNaN(heaterMaxTemp)) config.heater_max_temp = heaterMaxTemp;
    if (!isNaN(ntcLowTemp)) config.ntc_low_temp_threshold = ntcLowTemp;
    if (!isNaN(ntcHeatMinutes)) config.ntc_heat_on_minutes = ntcHeatMinutes;
    
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
    const feedIntervalMinutes = parseInt(document.getElementById('feedInterval').value);
    const feedDuration = parseInt(document.getElementById('feedDuration').value);
    const feedTimes = document.getElementById('feedTimes').value;
    
    // 從分鐘轉換為秒儲存
    if (!isNaN(feedIntervalMinutes)) config.feed_interval_seconds = feedIntervalMinutes * 60;
    if (!isNaN(feedDuration)) config.feed_duration_ms = feedDuration;
    if (feedTimes) config.feed_times_csv = feedTimes;
    
    // 系統設定
    const uploadIntervalMinutes = parseInt(document.getElementById('uploadInterval').value);
    const mode = document.getElementById('modeSelect').value;
    
    // 從分鐘轉換為秒儲存
    if (!isNaN(uploadIntervalMinutes)) config.upload_interval_seconds = uploadIntervalMinutes * 60;
    if (mode) config.mode = mode;
    
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
    document.getElementById('lastUpdate').textContent = timeString;
}

// ===== 定期檢查連接狀態 =====
setInterval(() => {
    if (mqttClient && !mqttClient.connected) {
        updateConnectionStatus(false);
    }
}, 5000);

// ===== 除錯工具函數 =====
window.mqttDebug = {
    resubscribe: function() {
        if (!mqttClient || !mqttClient.connected) {
            console.error('MQTT 未連接');
            return;
        }
        console.log('🔄 重新訂閱所有 Topics...');
        mqttClient.subscribe(topics.telemetry, {qos: 0}, (err) => {
            console.log(err ? '❌ 失敗:' : '✅ 成功:', topics.telemetry);
        });
        mqttClient.subscribe(topics.configOut, {qos: 0}, (err) => {
            console.log(err ? '❌ 失敗:' : '✅ 成功:', topics.configOut);
        });
        mqttClient.subscribe(topics.status, {qos: 0}, (err) => {
            console.log(err ? '❌ 失敗:' : '✅ 成功:', topics.status);
        });
    },
    checkTopics: function() {
        console.log('📋 當前 Topics:', topics);
        console.log('🔌 MQTT 連接狀態:', mqttClient ? mqttClient.connected : 'null');
        console.log('📱 裝置 ID:', deviceId);
    },
    testPublish: function() {
        if (!mqttClient || !mqttClient.connected) {
            console.error('MQTT 未連接');
            return;
        }
        const testTopic = 'farm/test/' + deviceId;
        mqttClient.publish(testTopic, 'test message from web');
        console.log('📤 已發送測試訊息到:', testTopic);
    }
};

