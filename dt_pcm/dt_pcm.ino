/*
 * speaker_pcm
 *
 * Plays 8-bit PCM audio on pin 11 using pulse-width modulation (PWM).
 * For Arduino with Atmega168 at 16 MHz.
 *
 * Uses two timers. The first changes the sample value 8000 times a second.
 * The second holds pin 11 high for 0-255 ticks out of a 256-tick cycle,
 * depending on sample value. The second timer repeats 62500 times per second
 * (16000000 / 256), much faster than the playback rate (8000 Hz), so
 * it almost sounds halfway decent, just really quiet on a PC speaker.
 *
 * Takes over Timer 1 (16-bit) for the 8000 Hz timer. This breaks PWM
 * (analogWrite()) for Arduino pins 9 and 10. Takes Timer 2 (8-bit)
 * for the pulse width modulation, breaking PWM for pins 11 & 3.
 *
 * References:
 *     http://www.uchobby.com/index.php/2007/11/11/arduino-sound-part-1/
 *     http://www.atmel.com/dyn/resources/prod_documents/doc2542.pdf
 *     http://www.evilmadscientist.com/article.php/avrdac
 *     http://gonium.net/md/2006/12/27/i-will-think-before-i-code/
 *     http://fly.cc.fer.hr/GDM/articles/sndmus/speaker2.html
 *     http://www.gamedev.net/reference/articles/article442.asp
 *
 * Michael Smith <michael@hurts.ca>
 */

#include <stdint.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>

#define SAMPLE_RATE 8000
//#include "dt_pcm.h"

/*
 * The audio data needs to be unsigned, 8-bit, 8000 Hz, and small enough
 * to fit in flash. 10000-13000 samples is about the limit.
 *
 * sounddata.h should look like this:
 *     const int sounddata_length=10000;
 *     const unsigned char sounddata_data[] PROGMEM = { ..... };
 *
 * You can use wav2c from GBA CSS:
 *     http://thieumsweb.free.fr/english/gbacss.html
 * Then add "PROGMEM" in the right place. I hacked it up to dump the samples
 * as unsigned rather than signed, but it shouldn't matter.
 *
 * http://musicthing.blogspot.com/2005/05/tiny-music-makers-pt-4-mac-startup.html
 * mplayer -ao pcm macstartup.mp3
 * sox audiodump.wav -v 1.32 -c 1 -r 8000 -u -1 macstartup-8000.wav
 * sox macstartup-8000.wav macstartup-cut.wav trim 0 10000s
 * wav2c macstartup-cut.wav sounddata.h sounddata
 *
 * (starfox) nb. under sox 12.18 (distributed in CentOS 5), i needed to run
 * the following command to convert my wav file to the appropriate format:
 * sox audiodump.wav -c 1 -r 8000 -u -b macstartup-8000.wav
 */

#include "sounddata.h"
int ledPin0 = 4; // RED
int ledPin1 = 5; // Yellow 1
int ledPin2 = 6; // Yellow 2
int ledPin3 = 7; // Yellow 3

int inputPin1 = 9; // RedPush Button (pulse)
//int inputPin2 = 8; // Switch close (perm)
//int inputPin3 = 9; // Switch Open (perm)


int seqPos = 1;
int sndPos = 0;


int speakerPin = 3; // Can be either 3 or 11, two PWM outputs connected to Timer 2
//int speakerPin = 11;

unsigned char const *sounddata_data=0;
int sounddata_length=0;

volatile uint16_t sample;
byte lastSample;

bool thisIsTheEnd = LOW;

// This is called at 8000 Hz to load the next sample.
ISR(TIMER1_COMPA_vect) {
  if (sample >= sounddata_length) {
    if (sample == sounddata_length + lastSample) {
      if (thisIsTheEnd == HIGH){
        stopPlayback();
        stopPlayback();
        if (sndPos == 1){
           sndPos = 3;
        }
      }else{
        sample = -1;
        OCR2B = sounddata_length + lastSample - sample;
        
        if (seqPos > 8){
          seqPos = 1;
        }else{
          ++seqPos;
        }
      }
    } else {
      // Ramp down to zero to reduce the click at the end of playback.
      OCR2B = sounddata_length + lastSample - sample;
    }
  }
  else {
    OCR2B = pgm_read_byte(&sounddata_data[sample]);
  }
  
  ++sample;
}

void startPlayback(unsigned char const *data, int length)
{
  sounddata_data = data;
  sounddata_length = length;

  pinMode(speakerPin, OUTPUT);
    // Set up Timer 2 to do pulse width modulation on the speaker
    // pin.

    // Use internal clock (datasheet p.160)
    ASSR &= ~(_BV(EXCLK) | _BV(AS2));

    // Set fast PWM mode  (p.157)
    TCCR2A |= _BV(WGM21) | _BV(WGM20);
    TCCR2B &= ~_BV(WGM22);

      // Do non-inverting PWM on pin OC2B (p.155)
      // On the Arduino this is pin 3.
      TCCR2A = (TCCR2A | _BV(COM2B1)) & ~_BV(COM2B0);
      TCCR2A &= ~(_BV(COM2A1) | _BV(COM2A0));
      // No prescaler (p.158)
      TCCR2B = (TCCR2B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);

      // Set initial pulse width to the first sample.
      OCR2B = pgm_read_byte(&sounddata_data[0]);
  cli();
  
  // Set CTC mode (Clear Timer on Compare Match) (p.133)
  // Have to set OCR1A *after*, otherwise it gets reset to 0!
  TCCR1B = (TCCR1B & ~_BV(WGM13)) | _BV(WGM12);
  TCCR1A = TCCR1A & ~(_BV(WGM11) | _BV(WGM10));
  
  // No prescaler (p.134)
  TCCR1B = (TCCR1B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);
  
  // Set the compare register (OCR1A).
  // OCR1A is a 16-bit register, so we have to do this with
  // interrupts disabled to be safe.
  OCR1A = F_CPU / SAMPLE_RATE;    // 16e6 / 8000 = 2000
  
  // Enable interrupt when TCNT1 == OCR1A (p.136)
  TIMSK1 |= _BV(OCIE1A);
  
  lastSample = pgm_read_byte(&sounddata_data[sounddata_length-1]);
  sample = 0;
  sei();
}

void stopPlayback()
{
  // Disable playback per-sample interrupt.
  TIMSK1 &= ~_BV(OCIE1A);
  
  // Disable the per-sample timer completely.
  TCCR1B &= ~_BV(CS10);
  
  // Disable the PWM timer.
  TCCR2B &= ~_BV(CS10);
  
  digitalWrite(speakerPin, LOW);
}

void setup()
{
    pinMode(ledPin0, OUTPUT);
    pinMode(ledPin1, OUTPUT);
    pinMode(ledPin2, OUTPUT);
    pinMode(ledPin3, OUTPUT);
    
    pinMode(inputPin1, INPUT);
    //pinMode(inputPin2, INPUT);
    //pinMode(inputPin3, INPUT);

    pinMode(speakerPin, OUTPUT);
    startPlayback(sounddata_bcle_data, sounddata_bcle_length);
}

void powerLed(bool A, bool B, bool C, bool D, unsigned int DL=0, unsigned int loopMe=1){
  while (loopMe > 0){
    if (loopMe%2 != 0){
      digitalWrite(ledPin0, A);
      digitalWrite(ledPin1, B);
      digitalWrite(ledPin2, C);
      digitalWrite(ledPin3, D);
    }else{
      digitalWrite(ledPin0, !A);
      digitalWrite(ledPin1, !B);
      digitalWrite(ledPin2, !C);
      digitalWrite(ledPin3, !D);
    }
    if (DL>0){
      delay(DL);
    }
    --loopMe;
  }
}

void loop()
{
    while (true){
      if (digitalRead(inputPin1) == LOW && thisIsTheEnd == LOW){
        thisIsTheEnd = HIGH;
        stopPlayback();
        sndPos = 1;
        startPlayback(sounddata_bug_data, sounddata_bug_length);
      }

      if (sndPos == 3){
            powerLed(HIGH, HIGH, HIGH,HIGH,200,20);
            sndPos = 0;
            startPlayback(sounddata_bcle_data, sounddata_bcle_length);
            thisIsTheEnd = LOW;
      }
      if (seqPos == 1){
        powerLed(HIGH, HIGH, LOW,LOW);
      }else if (seqPos == 2){
        powerLed(HIGH, HIGH, HIGH,LOW);
      }else if (seqPos == 3){
        powerLed(HIGH, HIGH, LOW,HIGH);
      }else if (seqPos == 4){
        powerLed(HIGH, LOW, HIGH,HIGH);
      }else if (seqPos == 5){
        powerLed(HIGH, LOW, HIGH,LOW);
      }else if (seqPos == 6){
        powerLed(HIGH, LOW, LOW,HIGH);
      }else if (seqPos == 7){
        powerLed(HIGH, HIGH, HIGH,LOW);
      }else if (seqPos == 8){
        powerLed(HIGH, LOW, HIGH,HIGH);
      }
    }
}


