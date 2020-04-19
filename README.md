# IsolatedAS3935
AS3935 Lightning Detector via MQTT
https://cmheong.blogspot.com/2020/04/as3935-lightning-detector-next.html

Server side:
mosquitto -c /etc/mosquitto/mosquitto.conf

To test relay module:
mosquitto_pub -t 'lightning/commands' -m 'modem_on'

To monitor the topics:
mosquitto_sub -t 'lightning/messages'
mosquitto_sub -t 'lightning/commands' 
