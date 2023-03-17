// $Id$
// CHU simulator program. Generates their audio program as closely as possible
// Even supports UT1 offsets and leap second insertion
// By default, uses system time, which should be NTP synchronized
// Time can be manually overridden for testing (announcements, leap seconds and other corner cases)

// Sept 2017, Phil Karn, KA9Q - adapted from wwvsim.c
// (Can you tell I have too much spare time?)

#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <memory.h>
#include <sys/time.h>
#include <locale.h>
#include <fcntl.h>

char Libdir[] = "/usr/local/share/ka9q-radio";

int Samprate = 48000; // Samples per second - try to use this if possible
int Samprate_ms;      // Samples per millisecond - sampling rates not divisible by 1000 may break

int Negative_leap_second_pending = 0; // If 1, leap second will be removed at end of June or December, whichever is first
int Positive_leap_second_pending = 0; // If 1, leap second will be inserted at end of June or December, whichever is first

// Is specified year a leap year?
int const is_leap_year(int y){
  if((y % 4) != 0)
    return 0; // Ordinary year; example: 2017
  if((y % 100) != 0)
    return 1; // Examples: 1956, 2004 (i.e., most leap years)
  if((y % 400) != 0)
    return 0; // Examples: 1900, 2100 (the big exception to the usual rule; non-leap US presidential election years)
  return 1; // Example: 2000 (the exception to the exception)
}

// Applies only to non-leap years; you need special tests for February in leap year
int const Days_in_month[] = { // Index 1 = January, 12 = December
  0,31,28,31,30,31,30,31,31,30,31,30,31
};

// Generate complex phasor with specified angle in radians
// Used for tone generation
complex double const csincos(double x){
  return cos(x) + I*sin(x);
}

int16_t *Audio; // Audio output buffer, 1 minute long

// Synthesize an audio announcement and insert it into the audio buffer at 'startms' milliseconds within the minute
// Use french = 1 for Canadian French, 0 for Canadian English
int announce(int startms,char const *message,int french){
  if(startms < 0 || startms >= 61000)
    return -1;

  // Overwrites buffer, so do early
  FILE *fp;
  if ((fp = fopen("/tmp/speakin","w")) == NULL)
    return -1;
  fputs(message,fp);
  fclose(fp);
#ifdef __linux__
  if(french){
    system("espeak -v en-us+f3 -a 70 -f /tmp/speakin --stdout | sox -t wav - -t raw -r 48000 -c 1 -e signed-integer -b 16 /tmp/speakout");
  } else {
    system("espeak -v en-us -a 70 -f /tmp/speakin --stdout | sox -t wav - -t raw -r 48000 -c 1 -e signed-integer -b 16 /tmp/speakout");
  }
#else // apple
  if(french){
    system("say -v Thomas --output-file=/tmp/speakout.wav --data-format=LEI16@48000 -f /tmp/speakin;sox /tmp/speakout.wav -t raw -r 48000 -c 1 -b 16 -e signed-integer /tmp/speakout");
  } else {
    system("say -v Alex --output-file=/tmp/speakout.wav --data-format=LEI16@48000 -f /tmp/speakin;sox /tmp/speakout.wav -t raw -r 48000 -c 1 -b 16 -e signed-integer /tmp/speakout");
  }
#endif

  unlink("/tmp/speakin");
  if((fp = fopen("/tmp/speakout","r")) == NULL)
    return -1;
  fread(Audio+startms*Samprate_ms,sizeof(*Audio),Samprate_ms*(61000-startms),fp);
  fclose(fp);
  unlink("/tmp/speakout");
  return 0;
}

// Overlay a tone with frequency 'freq' in audio buffer, overwriting whatever was there
// starting at 'startms' within the minute and stopping one sample before 'stopms'. 
// Amplitude 1.0 is 100% modulation, 0.5 is 50% modulation, etc
// Used first for 500/600 Hz continuous audio tones
// Then used for 1000/1200 Hz minute/hour beeps and second ticks, which pre-empt everything else.
int overlay_tone(int startms,int duration,float freq,float amp){
  if(startms < 0 || startms >= 61000 || duration < 0)
    return -1;

#if 0
  // Unlike WWV, the modem tones don't start cleanly
  assert((startms * (int)freq % 1000) == 0); // All tones start with a positive zero crossing?
#endif

  complex double phase = 1;
  complex double const phase_step = csincos(2*M_PI*freq/Samprate);
  int16_t *buffer = Audio + startms*Samprate_ms;
  int samples = duration*Samprate_ms;
  while(samples-- > 0){
    *buffer++ = cimag(phase) * amp * SHRT_MAX; // imaginary component is sine, real is cosine
    phase *= phase_step;  // Rotate the tone phasor
  }
 return 0;
}

// Blank out whatever is in the audio buffer starting at startms and ending just before stopms
// Used mainly to blank out 40 ms guard interval around seconds ticks
int overlay_silence(int startms,int stopms){
  if(startms < 0 || startms >= 61000 || stopms <= startms || stopms > 61000)
    return -1;
  int16_t *buffer = Audio + startms*Samprate_ms;
  int samples = (stopms - startms)*Samprate_ms;

  while(samples-- > 0)
    *buffer++ = 0;

  return 0;
}

int Verbose = 0;
int Quiet = 0; // Tones by default;

int main(int argc,char *argv[]){
  int c;
  int year,month,day,hour,minute,sec,doy;
  // Amplitudes
  const float tick_amp = 1.0; // 100%, 0dBFS
  const float tone_amp = pow(10.,-6.0/20.); // -6 dB

  // Defaults
  double tickfreq = 1000.0; // WWV
  int dut1 = 0;
  int manual_time = 0;
  double Offset = 0;
  
  // Use current computer time as default
  struct timeval startup_tv;
  gettimeofday(&startup_tv,NULL);
  struct tm const * const tm = gmtime(&startup_tv.tv_sec);
  sec = tm->tm_sec;
  minute = tm->tm_min;
  hour = tm->tm_hour;
  day = tm->tm_mday;
  month = tm->tm_mon + 1;
  year = tm->tm_year + 1900;
  doy = tm->tm_yday + 1;
  setlocale(LC_ALL,getenv("LANG"));

  // Read and process command line arguments
  while((c = getopt(argc,argv,"qHY:M:D:h:m:s:u:r:LNvo:")) != EOF){
    switch(c){
    case 'q':
      Quiet++; // no tones
      break;
    case 'o':
      // Time offset, milliseconds, to account for audio delay
      // Positive adds delay, negative advances it
      Offset = strtod(optarg,NULL);
      break;
    case 'v':
      Verbose++;
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0); // Try not to change this, may not work
      break;
    case 'u': // UT1 offset in tenths of a second, +/- 7
      dut1 = strtol(optarg,NULL,0);
      break;
    case 'Y': // Manual year setting
      year = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 'M': // Manual month setting
      month = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 'D': // Manual day setting
      day = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 'h': // Manual hour setting
      hour = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 'm': // Manual minute setting
      minute = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 's': // Manual second setting
      sec = strtol(optarg,NULL,0);
      manual_time++;
      break;
    case 'L':
      Positive_leap_second_pending++; // Positive leap second at end of current month
      break;
    case 'N':
      Negative_leap_second_pending++;  // Leap second at end of current month
      break;
    case '?':
      fprintf(stderr,"Usage: %s [-v] [-r samprate] [-u ut1offset] [-Y year] [-M month] [-D day] [-h hour] [-m min] [-s sec] [-L|-N]\n",argv[0]);
      fprintf(stderr,"Default sample rate: 48 kHz\n");
      fprintf(stderr,"By default uses current system time; Use -Y/-M/-D/-h/-m/-s to override for testing, e.g., of leap seconds\n");
      fprintf(stderr,"-v turns on verbose reporting. -H selects the WWVH format; default is WWV\n");
      fprintf(stderr,"-u specifies current UT1-UTC offset in tenths of a second, must be between -7 and +7\n");
      fprintf(stderr,"-L introduces a positive leap second at the end of June or December, whichever comes first\n");
      fprintf(stderr,"-N introduces a negative leap second at the end of June or December, whichever comes first. Only one of -L and -N can be given\n");

      exit(1);
	      
    }	
  }
  if(isatty(fileno(stdout))){
    fprintf(stderr,"Won't write raw PCM audio to a terminal. Redirect or pipe.\n");
    exit(1);
  }

  if(Positive_leap_second_pending && Negative_leap_second_pending){
    fprintf(stderr,"Positive and negative leap seconds can't both be pending! Both cancelled\n");
    Positive_leap_second_pending = Negative_leap_second_pending = 0;
  }

  if(dut1 > 7 || dut1 < -7){
    fprintf(stderr,"ut1 offset %d out of range, limited to -7 to +7 tenths\n",dut1);
    dut1 = 0;
  }
  Audio = malloc(Samprate*61*sizeof(int16_t));
  Samprate_ms = Samprate/1000; // Samples per ms

  // Only US rules are needed, since WWV/WWVH are American stations
  // US rules last changed in 2007 to 2nd sunday of March to first sunday in November
  // Always lasts for 238 days
  // 2007: 3/11      2008: 3/9      2009: 3/8      2010: 3/14      2011: 3/13      2012: 3/11
  // 2013: 3/10      2014: 3/9      2015: 3/8      2016: 3/13      2017: 3/12      2018: 3/11
  // 2019: 3/10      2020: 3/8

  int dst_start_doy = 0;
  if(year < 2007){
    // Punt
    fprintf(stderr,"Warning: DST rules prior to %d not implemented; DST bits = 0\n",year);
  } else {
    int ytmp = 2007;
    int dst_start_dom = 11;
    for(;ytmp<= year;ytmp++){
      dst_start_dom--; // One day earlier each year
      if(is_leap_year(ytmp))
	dst_start_dom--; // And an extra day earlier after a leap year february
      if(dst_start_dom <= 7) // No longer second sunday, slip a week
	dst_start_dom += 7;
    }
    dst_start_doy = 59 + dst_start_dom;
    if(is_leap_year(year))
      dst_start_doy++;
  }

  int samplenum = Samprate_ms * Offset; // Initial samples to trim
  if(!manual_time){
    // Find time interval since startup, trim that many samples from the beginning of the buffer so we are on time
    struct timeval tv;
    gettimeofday(&tv,NULL);
    int startup_delay = 1000000*(tv.tv_sec - startup_tv.tv_sec) +
      tv.tv_usec - startup_tv.tv_usec;
    
    fprintf(stderr,"startup delay %'d usec\n",startup_delay);
    samplenum += (Samprate_ms * startup_delay) / 1000;
    manual_time = 1; // do this only first time
  }

  while(1){
    int length = 60;    // Default length 60 seconds
    if((month == 6 || month == 12) && hour == 23 && minute == 59){
      if(Positive_leap_second_pending){
	length = 61; // This minute ends with a leap second!
      } else if(Negative_leap_second_pending){
	length = 59; // Negative leap second
      }
    }
    fprintf(stderr,"%d/%d/%d %02d:%02d:%02d",month,day,year,hour,minute,sec);
    if(length != 60)
      fprintf(stderr,"Leap second at end of this minute!");
    
    fprintf(stderr,"\n");
    fprintf(stderr,"total offset %.0f ms (%s)\n",Offset,Offset > 0 ? "early" :
	   Offset < 0 ? "late" : "on time");
    // Build a minute of audio
    memset(Audio,0,61*Samprate*sizeof(*Audio)); // Clear previous audio

    // Insert minute announcement
    int nextminute,nexthour; // What are the next hour and minute?
    nextminute = minute;
    nexthour = hour;
    if(++nextminute == 60){
      nextminute = 0;
      if(++nexthour == 24)
	nexthour = 0;
    }
    char message[1024];
    if((nextminute % 2) == 0){
      // English first
      snprintf(message,sizeof(message),"CHU Canada Coordinated Universal Time, %d %s %d %s",
	       nexthour,nexthour == 1 ? "hour" : "hours",
	       nextminute,nextminute == 1 ? "minute" : "minutes");
      announce(50000,message,0);
      snprintf(message,sizeof(message),"%d %s %d %s",
	       nexthour,nexthour == 1 ? "heure" : "heures",
	       nextminute,nextminute == 1 ? "minute" : "minutes");
      announce(57000,message,1);
    } else {
      // Francais first
      snprintf(message,sizeof(message),"CHU Canada U T C, %d %s %d %s",
	       nexthour,nexthour == 1 ? "heure" : "heures",	       
	       nextminute,nextminute == 1 ? "minute" : "minutes");
      announce(50000,message,1);
      snprintf(message,sizeof(message),"%d %s %d %s",
	       nexthour,nexthour == 1 ? "hour" : "hours",
	       nextminute,nextminute == 1 ? "minute" : "minutes");
      announce(57000,message,0);
    }

    if(minute == 0){
      // 1 sec tone, 9 sec silence, then beeps
      overlay_tone(0,1000,tickfreq,tick_amp);
      overlay_silence(1000,10000);
      int s;
      for(s=10;s<29;s++)
	overlay_tone(1000*s,300,tickfreq,tick_amp);
      for(s=30;s<50;s++)
	overlay_tone(1000*s,300,tickfreq,tick_amp);
      for(s=50;s<59;s++)
	overlay_tone(1000*s,10,tickfreq,tick_amp);
    } else {
      overlay_tone(0,500,tickfreq,tick_amp);
      int s;
      for(s=1;s<29;s++){
	// Double ticks without guard time for UT1 offset
	if((dut1 > 0 && s >= 1 && s <= dut1)
	   || (-dut1 > 0 && s >= 9 && s <= 8-dut1)){
	  overlay_tone(1000*s,125,tickfreq,tick_amp);
	  overlay_tone(1000*s+175,125,tickfreq,tick_amp);
	} else {
	  overlay_tone(1000*s,300,tickfreq,tick_amp);
	}
      }
      overlay_tone(1000*30,300,tickfreq,tick_amp);
      for(s=31;s<40;s++){
	overlay_tone(1000*s,10,tickfreq,tick_amp);
	overlay_tone(1000*s+10,123,2225,tick_amp);
	overlay_tone(1000*s+133,367,2025,tick_amp);
	overlay_tone(1000*s+500,10,2225,tick_amp);
      }
      for(s=40;s<50;s++)
	overlay_tone(1000*s,300,tickfreq,tick_amp);

      for(s=50;s<59;s++)
	overlay_tone(1000*s,10,tickfreq,tick_amp);

    }
    // Write the constructed buffer, minus startup delay plus however many seconds
    // have already elapsed since the minute. This happens only at startup;
    // on all subsequent minutes the entire buffer will be written
    if(samplenum < 0){
      fwrite(Audio,sizeof(*Audio),samplenum,stdout);
      samplenum = 0;
    }
    fwrite(Audio+samplenum+sec*Samprate,sizeof(*Audio),
	   Samprate * (length-sec) - samplenum,stdout);

    samplenum = 0; // clear for next and subsequent minutes

    if(length == 61){
      // Leap second just occurred in this last minute
      Positive_leap_second_pending = 0;
      dut1 += 10;
    } else if(length == 59){
      Negative_leap_second_pending = 0;
      dut1 -= 10;
    }
    // Advance to next minute
    sec = 0;
    if(++minute > 59){
      // New hour
      minute = 0;
      if(++hour > 23){
	// New day
	hour = 0;
	doy++;
	if(++day > ((month == 2 && is_leap_year(year))? 29 : Days_in_month[month])){
	  // New month
	  day = 1;
	  if(++month > 12){
	    // New year
	    month = 1;
	    ++year;
	    doy = 1;
	  }
	}
      }
    }
  }
}

