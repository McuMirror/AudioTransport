/*
 *********************    Codec2 in ESP32    ********************

 This test program implement the encoder and decoder of Codec2
 at 1600bps using LoRa radio.

 Codec 2 is a low-bitrate speech audio codec (speech coding) 
 that is patent free and open source develop by David Grant Rowe.
 http://www.rowetel.com/
 
 This program samples the audio in the transmitter at 8KHz using 
 an ADC a reproduces it in the receiver using a DAC.

 Every 40ms will be generate a new codec2 encoded frame with 8 bytes, 
 then every 5 codec2 frames will be generate a transmission frame.
 In this schema a transmission happened at 200ms intervals, so you 
 have less than 200ms to make the transmission (I'm using 182ms).

 In this implementation the transmission frame has 44 bytes, the 
 first 4 bytes are the header and the others are the voice.
 You can use the header to indicate the address of the transmitter, 
 the address of the desire receiver, etc.

 

 ***********************   W A R N I N G   ********************
 
 This test program DO NOT complies with FCC regulations from USA 
 even not complies with ANATEL from Brazil and I guess do not 
 complies with any regulations from any country.

 To complies with the FCC and orders foreign agencies, normally 
 you need to implement a frequency hopping algorithm that is 
 outside the scope of this test program.

 Please verify your country regulations. You assume all 
 responsibility for the use or misuse of this test program.

 TIP: 
 The challenge of a frequency hopping system is the synchronization, 
 maybe you can use a GPS receiver for synchronization.
 
 Use coddec2 library from this link https://github.com/LieBtrau/digital-walkie-talkie/tree/master/firmware/tests/audio/esp32-codec2/lib/Codec2/src
 
 **************************************************************
*/

#include <Arduino.h>
#include <driver/adc.h>

#include <esp_now.h>
#include <WiFi.h>

#define ADC_PIN ADC1_CHANNEL_0          // ADC 1 channel 0 GPIO36
#define lineOut 25
#define PTT_PIN 2

//#define FREQUENCY  915E6
#define ADC_BUFFER_SIZE 320             // 40ms of voice in 8KHz sampling frequency
#define ENCODE_FRAME_SIZE 44            // 44 =  First four bytes non-audio + 40 bytes audio
#define ENCODE_CODEC2_FRAME_SIZE 8      // 8 bytes per packets x 5 packets = 40 bytes of audio packet
#define ENCODE_FRAME_HEADER_SIZE 4      // 4 bytes of non-audio data

int16_t adc_buffer[ADC_BUFFER_SIZE];                   // 320 = 40ms of voice in 8KHz sampling frequency
int16_t speech[ADC_BUFFER_SIZE];                       // 320 = 40ms of voice in 8KHz sampling frequency
int16_t output_buffer[ADC_BUFFER_SIZE];                // 320 = 40ms of voice in 8KHz sampling frequency
uint8_t transmitBuffer[ADC_BUFFER_SIZE];               // 320 = 40ms of voice in 8KHz sampling frequency
unsigned char rx_encode_frame[ENCODE_FRAME_SIZE];      // 44 =  First four bytes non-audio + 40 bytes audio
unsigned char tx_encode_frame[ENCODE_FRAME_SIZE];      // 44 bytes
int tx_encode_frame_index = 0;
uint8_t rx_raw_audio_value = 127;                      // No audio value (middle)
int adc_buffer_index = 0;



// REPLACE WITH YOUR RECEIVER MAC Address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};



//#include <SPI.h>
//#include <LoRa.h>               //https://github.com/sandeepmistry/arduino-LoRa

#include <codec2.h>				      //In the codec2 folder in the library folder
#include <ButterworthFilter.h>	//In the codec2 folder in the library folder
#include <FastAudioFIFO.h>		  //In the codec2 folder in the library folder
/*
#define SCK     5    // GPIO5  -- SX1278's SCK
#define MISO    19   // GPIO19 -- SX1278's MISnO
#define MOSI    27   // GPIO27 -- SX1278's MOSI
#define SS      18   // GPIO18 -- SX1278's CS
#define RST     14   // GPIO14 -- SX1278's RESET
#define DI0     26   // GPIO26 -- SX1278's IRQ(Interrupt Request)
*/

/*
//int16_t 1KHz sine test tone
int16_t Sine1KHz[8] = { -21210 , -30000, -21210, 0 , 21210 , 30000 , 21210, 0 };
int Sine1KHz_index = 0;
*/

FastAudioFIFO audio_fifo;

enum RadioState
{
	radio_standby, radio_rx, radio_tx 
};
volatile RadioState radio_state = RadioState::radio_tx;



//The codec2 
struct CODEC2* codec2_state;

//Implement a high pass 240Hz Butterworth Filter.
ButterworthFilter hp_filter(240, 8000, ButterworthFilter::ButterworthFilter::Highpass, 1);

hw_timer_t* adcTimer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t codec2HandlerTask;


////////////////////////////////////End of Variables///////////////////////////////////////////////////////////////////////////





/////////////////////////////////////Start of event handler functions//////////////////////////////////////////////////////////////

void OnDataRecv(const uint8_t * mac, const uint8_t *data, int len) {
  //Serial.print("bytes received: ");
  //Serial.print(len);
  //Set the state to radio_rx because we are receiving
    radio_state = RadioState::radio_rx;
    //Serial.println("Receiving audio packets...");
    
    // Notify run_codec2 task that we have received a new packet.
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(codec2HandlerTask, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
      portYIELD_FROM_ISR();
    }
    
  uint8_t audioIn;
  for (int i=0;i<len;i++) {
    audioIn += (uint8_t)data[i];
    dacWrite(lineOut, audioIn);
  //Serial.print("Audio data in: ");
  //Serial.println(audioIn);
 }
}



/*
// OnTxDone event handler
void onTxDone() 
{
//	slapsed_tx = millis() - start_tx; //Just for debug
//	LoRa.receive(); //The transmission is done, so let's be ready for reception
	Serial.println("Waiting for audio packets...");
 
//	tx_ok = true;
}
*/
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
// onReceive event handler
void onReceive(int packetSize) 
{
  if (packetSize == ENCODE_FRAME_SIZE)// We received a voice packet
  {
    //read the packet
    for (int i = 0; i < packetSize; i++) 
  //    rx_encode_frame[i] = LoRa.read();
      
    //Set the state to radio_rx because we are receiving
    radio_state = RadioState::radio_rx;
    Serial.println("Receiving audio packets...");
    
    // Notify run_codec2 task that we have received a new packet.
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(codec2HandlerTask, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
      portYIELD_FROM_ISR();
    }
  }
}


// onReceive event handler
void onReceive() 
{
 
   //Set the state to radio_rx because we are receiving
    radio_state = RadioState::radio_rx;
    Serial.println("Receiving audio packets...");
    
    // Notify run_codec2 task that we have received a new packet.
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(codec2HandlerTask, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
      portYIELD_FROM_ISR();
    }
  }
*/

////////////////////////////////////////End of event handler functions/////////////////////////////////////////////////////////////////////

////////////////////////////////////////Start of timer function/////////////////////////////////////////////////////////////////////

void IRAM_ATTR onTimer() {
	portENTER_CRITICAL_ISR(&timerMux);                //Enter crital code without interruptions

/////////////////////////////////Transmit Audio from microphone////////////////////////////////////////////////////////////////////////////

	if (radio_state == RadioState::radio_tx)          // Microphone is open and active
	{
		//Read the ADC and convert it's value from (0 - 4095) to (-32768 - 32767) = 16 x 4095 = 65520 total
		adc_buffer[adc_buffer_index++] = (16 * adc1_get_raw(ADC1_CHANNEL_0)) - 32768;  // Input voice data from microphone and store it to int16_t array called adc_buffer

		
		//When buffer is full
		if (adc_buffer_index == ADC_BUFFER_SIZE) {      // i.e. adc_buffer array has 240 bytes
			adc_buffer_index = 0;

			//slapsed_in = millis() - last_tick; //Just for debug
			//last_tick = millis();              //Just for debug

			//Transfer the buffer from adc_buffer to speech buffer
			memcpy((void*)speech, (void*)adc_buffer, 2 * ADC_BUFFER_SIZE);  // Store 240 bytes of microphone voice data array to int16_t array called speech

			// Notify run_codec2 task that the buffer is ready.
			BaseType_t xHigherPriorityTaskWoken = pdFALSE;
			vTaskNotifyGiveFromISR(codec2HandlerTask, &xHigherPriorityTaskWoken);
			if (xHigherPriorityTaskWoken)
			{
				portYIELD_FROM_ISR();
			}
		}
	}
	
////////////////////////Create audio & write to DAC///////////////////////////////////////////////////////////////////////////
	
	else if (radio_state == RadioState::radio_rx)
	{
		int16_t v;
    
    if (audio_fifo.get(&v))                               // Get audio data to send it to DAC to play
			rx_raw_audio_value = (uint8_t)((v + 32768) / 256);  // Store audio data in 0-255 values to uint8_t integer type variable
      dacWrite(lineOut, rx_raw_audio_value); // Play stream from uint8_t integer type variable values (audio data)
	//    Serial.print(rx_raw_audio_value);
	}
	portEXIT_CRITICAL_ISR(&timerMux); // exit critical code
}

////////////////////////////////////////End of timer function/////////////////////////////////////////////////////////////////////

void run_codec2(void* parameter)          // This function is called from setup
{
	//Init codec2
	codec2_state = codec2_create(CODEC2_MODE_1600);
	codec2_set_lpc_post_filter(codec2_state, 1, 0, 0.8, 0.2);

	//long start_encoder, start_decoder; //just for debug
	
	

	// Header, you have 4 bytes of header in each frame, you can use it for you protocol implementation
	// for instace indicate address of the transmiter and address of the desire receiver, etc.
	
	tx_encode_frame[0] = 0x00;
	tx_encode_frame[1] = 0x00;
	tx_encode_frame[2] = 0x00;
	tx_encode_frame[3] = 0x00;
	
	RadioState last_state = RadioState::radio_standby;
	while (1)
	{
		//Wait until be notify or 1 second
		uint32_t tcount = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

		if (tcount != 0) //if the task was notified! 
		{
			//Init the tx_encode_frame_index if a trasmition start
			if (radio_state != last_state)
			{
				if (radio_state == RadioState::radio_tx)
				{
					tx_encode_frame_index = ENCODE_FRAME_HEADER_SIZE;  // i.e. 4 bytes
				}
				last_state = radio_state;
			}

/////////////////Trasmitting audio data from microphone/////////////////////////////////////////////////////////
			
			if (radio_state == RadioState::radio_tx)    // Trasnmitting - Microphone open and active
			{
				// start_encoder = millis();              //Just for debug

				//Apply High Pass Filter
				for (int i = 0; i < ADC_BUFFER_SIZE; i++)
					speech[i] = (int16_t)hp_filter.Update((float)speech[i]);

				//encode the 320 bytes(40ms) of speech frame into 8 bytes per ms
				codec2_encode(codec2_state, tx_encode_frame + tx_encode_frame_index, speech);	

				//increment the pointer where the encoded frame must be saved
				tx_encode_frame_index += ENCODE_CODEC2_FRAME_SIZE;   // 8 bytes

				//slapsed_encoder = millis() - start_encoder; //Just for debug

				//If it is the 5th time then we have a ready trasnmission frame
				if (tx_encode_frame_index == ENCODE_FRAME_SIZE)      // 44 bytes
				{
					//start_tx = millis(); //Just for debug
					tx_encode_frame_index = ENCODE_FRAME_HEADER_SIZE;  // increment the pointer at 4 bytes
        /*
					//Transmit it
					LoRa.beginPacket();
					LoRa.write(tx_encode_frame, ENCODE_FRAME_SIZE);  // Transmit 44 bytes of packet
					LoRa.endPacket(true);
        */
				}
			}

//////////////////////////////////////receiving audio packet & storing it to buffer///////////////////////////////////////////////////////
     
			if (radio_state == RadioState::radio_rx) // Receiving
			{
				//start_decoder = millis(); //Just for debug

				//Make a cycle to get each codec2 frame from the received frame
				for (int i = ENCODE_FRAME_HEADER_SIZE; i < ENCODE_FRAME_SIZE; i += ENCODE_CODEC2_FRAME_SIZE)
				{
					//memcpy((void*)rx_encode_frame, (void*)tx_encode_frame, 2 * ADC_BUFFER_SIZE);  // Create test loop to play microphone voice data from ADC to Speaker connected to DAC
					
					//Decode the codec2 frame
					codec2_decode(codec2_state, output_buffer, rx_encode_frame + i);  // decoded audio stored to int16_t array of 320 bytes called output buffer
					
					// Add to the audio buffer the 320 samples resulting of the decode of the codec2 frame.
					for (int g = 0; g < ADC_BUFFER_SIZE; g++)
						audio_fifo.put(output_buffer[g]);      
				}

				//slapsed_decoder = millis() - start_decoder; //Just for debug
				//rx_ok = true;				
			}
		}
	}
}

///////////////////////////////////////////Start of setup////////////////////////////////////////////////////////////////////

void setup() {
	Serial.begin(115200); 

   // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

 esp_now_register_recv_cb(OnDataRecv);
 
  // Register peer
  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  
  // Add peer        
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
  
/*
	SPI.begin(SCK, MISO, MOSI, SS);
	LoRa.setPins(SS, RST, DI0);

	if (!LoRa.begin(FREQUENCY)) 
	{
		Serial.println("Starting LoRa failed!");
		while (1);
	}

	LoRa.setSpreadingFactor(9);
	LoRa.setSignalBandwidth(250E3);
	LoRa.setCodingRate4(7); // 4/7
	LoRa.setPreambleLength(6);
	LoRa.setSyncWord(0x12);
	LoRa.enableCrc();
	LoRa.onTxDone(onTxDone);
	LoRa.onReceive(onReceive);
  */


	adc1_config_width(ADC_WIDTH_12Bit);
	adc1_config_channel_atten(ADC_PIN, ADC_ATTEN_DB_6); //ADC 1 channel 0 (GPIO36).

	//Start the task that run the coder and decoder
	xTaskCreate(&run_codec2, "codec2_task", 30000, NULL, 5, &codec2HandlerTask);
  Serial.println("Codec2 encoder & decoder Started....");
	
	//Start a timer at 8kHz to sample the ADC and play the audio on the DAC.
	adcTimer = timerBegin(3, 500, true);            // 80 MHz / 500 = 160KHz MHz hardware clock
	timerAttachInterrupt(adcTimer, &onTimer, true); // Attaches the handler function to the timer 
	timerAlarmWrite(adcTimer, 20, true);            // Interrupts when counter == 20, 8.000 times a second
	timerAlarmEnable(adcTimer);                     // Activate it

	//last_tick = millis(); //Just for debug

	//Configure PTT input button
	pinMode(PTT_PIN, INPUT_PULLUP);

	//Set state 
	radio_state = RadioState::radio_rx;
	Serial.println("Waiting to receive audio packets...");
}

//////////////////////////////////////End of setup//////////////////////////////////////////////////////////////////




void loop() {
  // Serial.println(touchRead(4));
	if (digitalRead(PTT_PIN) == LOW || touchRead(4) < 30)
	{
		
		radio_state = RadioState::radio_tx;
    //strcpy(myData.voiceData, tx_encode_frame);
    //myData.voiceData = tx_encode_frame;
  
  // Send message via ESP-NOW
  //esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) tx_encode_frame, sizeof(tx_encode_frame)); 
  
  if (result == ESP_OK) {
  //  Serial.println("Sent with success");
  }
  else {
    Serial.println("Error sending the data");
  }
 
  //  Serial.println("Transmitting audio packets...");
	}
	else //if (tx_ok)
	{
		radio_state = RadioState::radio_rx;
  //  Serial.println("Waiting for audio packets...");
	}

/*
	//Some DEBUG stuffs, you can remove it if you want.
	if (rx_ok)
	{
		Serial.print(rx_ok_counter);
		Serial.print(" Dec=");
		Serial.println(slapsed_decoder);
		rx_ok_counter++;
		rx_ok = false;
	}

	if (tx_ok)
	{
		Serial.print(tx_ok_counter);
		Serial.print(" Enc=");
		Serial.print(slapsed_encoder);
		Serial.print(" Tx=");
		Serial.println(slapsed_tx);		
		tx_ok_counter++;
		tx_ok = false;
	}

	*/

	delay(1);//At least 1ms please!
}