#include <CoreFoundation/CoreFoundation.h>
#include <CoreMidi/CoreMidi.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#define FRAME_SIZE_NS 20000000
#define PLAYING_MAX 1024
#define INBUF_SIZE 1024
#define PACKET_LIST_SIZE 4096
#define DEFAULT_USPQ 500000 // 120 bpm
#define GARBAGE_SIZE 32

struct sequencerEvent {
  uint32_t tick;
  uint64_t atNs;
  uint8_t typeChan;
  uint8_t arg1;
  uint8_t arg2;
};

struct tempoChange {
  uint32_t tick;
  uint64_t atNs;
  uint32_t uspq; // microseconds per quarter note
};

struct sequence {
  int eventCount;
  int tempoChangeCount;
  struct sequencerEvent* events;
  struct tempoChange* tempoChanges;
};

struct playingNote {
  unsigned char playing : 1;
  unsigned char channel : 4;
  unsigned char note : 7;
};

MIDIClientRef client;
MIDIEndpointRef inputPort;
MIDIEndpointRef outputPort;
pthread_t dispatchThread;

int playFlag = 0;
uint64_t absolutePlayHeadNs;
uint64_t absoluteLeadingEdgeNs;
uint64_t absoluteSongStartNs;
uint64_t songNs = 0;

int onlineSeekFlag = 0;
uint64_t onlineSeekTargetNs;
int cutAllFlag = 0;

int loopFlag = 0;
int loopInitialized = 0;
uint64_t loopStartNs;
uint64_t loopEndNs;
double loopStartBeat;
double loopEndBeat;

uint32_t ticksPerBeat = 384;

struct sequence* currentSequence = NULL;
struct sequence* garbage[GARBAGE_SIZE];

pthread_mutex_t garbageMutex;
pthread_cond_t garbageSignal;

void trashSequence(struct sequence* seq){
  int i;
  for(i=0; i<GARBAGE_SIZE; i++){
    if(garbage[i] == NULL){
      garbage[i] = seq;
      return;
    }
  }

  fprintf(stderr, "** SOUND garbage has piled up\n");
  exit(-1);
}

void emptyTrash(){
  pthread_cond_signal(&garbageSignal);
}

uint64_t beatToNs(double beat){
  struct sequence* seqSnap = currentSequence;
  struct tempoChange* tempoChanges = seqSnap->tempoChanges;
  int tempoChangeCount = seqSnap->tempoChangeCount;
  uint32_t targetTick = beat * ticksPerBeat;
  uint32_t uspq;
  uint32_t baseTick;
  int i;
  if(tempoChangeCount == 0 || targetTick < tempoChanges[i].tick){
    uspq = DEFAULT_USPQ;
    baseTick = 0;
  }
  else{
    for(i=0; i<tempoChangeCount-1 && targetTick > tempoChanges[i].tick; i++);
    uspq = tempoChanges[i].uspq;
    baseTick = tempoChanges[i].tick;
  }
  return (targetTick-baseTick)*1000.0*uspq/ticksPerBeat;
}


void setLoopEndpoints(double loop0, double loop1){
  loopStartBeat = loop0;
  loopEndBeat = loop1;
  loopStartNs = beatToNs(loop0);
  loopEndNs = beatToNs(loop1);
  loopInitialized = 1;
}

double getCurrentBeat(){
  struct sequence* seqSnap = currentSequence;
  struct tempoChange* tempoChanges = seqSnap->tempoChanges;
  int tempoChangeCount = seqSnap->tempoChangeCount;
  uint32_t uspq;
  uint32_t baseNs;
  uint64_t songNsSnap = songNs;
  int i;
  if(tempoChangeCount == 0 || songNsSnap < tempoChanges[i].atNs){
    uspq = DEFAULT_USPQ;
    baseNs = 0;
  }
  else{
    for(i=0; i<tempoChangeCount-1 && songNsSnap > tempoChanges[i].atNs; i++);
    uspq = tempoChanges[i].uspq;
    baseNs = tempoChanges[i].atNs;
  }
  return (songNsSnap - baseNs)/(1000.0 * uspq);
}

void executeSeek(int number, int numerator, int denominator){
  struct sequence* seqSnap = currentSequence;
  double beat = number + (double)numerator / denominator;
  uint32_t targetTick = beat * ticksPerBeat;
  uint64_t targetNs;
  uint32_t uspq;
  uint32_t baseTick;
  int i;
  struct tempoChange* tempoChanges = seqSnap->tempoChanges;
  int tempoChangeCount = seqSnap -> tempoChangeCount;
  if(tempoChangeCount == 0 || targetTick < tempoChanges[i].tick){
    uspq = DEFAULT_USPQ;
    baseTick = 0;
  }
  else{
    for(i=0; i<tempoChangeCount-1 && targetTick > tempoChanges[i].tick; i++);
    uspq = tempoChanges[i].uspq;
    baseTick = tempoChanges[i].tick;
  }
  targetNs = (targetTick-baseTick)*1000.0*uspq/ticksPerBeat;
  if(playFlag == 0){
    songNs = targetNs;
  }
  else{
    onlineSeekFlag = 1;
    onlineSeekTargetNs = targetNs;
    usleep(FRAME_SIZE_NS/1000);
  }
}

struct playingNote playingNotes[PLAYING_MAX];
int playingCount = 0;

void initPlayingNotes(){
  int i;
  for(i=0; i<PLAYING_MAX; i++){
    playingNotes[i].playing = 0;
  }
}


void rememberNoteOn(int channel, int note){
  int i;
  for(i=0; i<PLAYING_MAX && playingNotes[i].playing==1; i++);
  if(i >= PLAYING_MAX){
    fprintf(stderr, "** SOUND remembering too many on-notes\n");
    exit(-1);
  }
  playingNotes[i].playing = 1;
  playingNotes[i].channel = channel;
  playingNotes[i].note = note;
  playingCount++;
}

void forgetNoteOn(int channel, int note){
  int i = 0;
  int count = 0;
  for(;;){
    if(i >= PLAYING_MAX) break;
    if(count >= playingCount) break;
    if(playingNotes[i].playing == 1){
      count++;
      if(playingNotes[i].channel == channel && playingNotes[i].note == note){
        playingNotes[i].playing = 0;
        playingCount--;
        break;
      }
    }
    i++;
  }
}

// cut all playing notes
void killAll(){
  unsigned char packetListStorage[PACKET_LIST_SIZE];
  MIDIPacketList* packetList = (MIDIPacketList*) packetListStorage;
  MIDIPacket* packet;
  unsigned char midi[3];
  uint64_t timeOfCut = absoluteLeadingEdgeNs;
  int count = 0;
  int i = 0;

  packet = MIDIPacketListInit(packetList);
  for(;;){
    if(count >= playingCount) break;
    if(i >= PLAYING_MAX) break;
    if(playingNotes[i].playing){
      midi[0] = 0x80 | playingNotes[i].channel;
      midi[1] = playingNotes[i].note;
      midi[2] = 0;
      packet = MIDIPacketListAdd(
        packetList,
        PACKET_LIST_SIZE,
        packet,
        timeOfCut,
        3,
        midi
      );
      if(packet == NULL){
        fprintf(stderr, "** SOUND unable to MIDIPacketListAdd (cut all)\n");
        exit(-1);
      }
      playingNotes[i].playing = 0;
      count++;
    }
    i++;
  }
  playingCount = 0;
  MIDIReceived(outputPort, packetList);
}



/** the capture buffer **/
/*
#define CAPTURE_SIZE 65536
char captureBuf[CAPTURE_SIZE];
int captureRead = 0;
int captureWrite = 0;
// copy as much out of capture buffer as possible, return count copied
int ringRead(char* dest){
  int ptr = captureWrite;
  int count;
  int count1;
  int count2;
  if(captureRead == ptr){ // buffer empty
    return 0;
  }
  else if(captureRead < ptr) { // no need to wrap
    count = ptr - captureRead;
    memcpy(dest, captureBuf + captureRead, count);
    captureRead = ptr;
    return count;
  }
  else { // need to copy twice to account for wrap
    count1 = CAPTURE_SIZE - captureRead;
    count2 = ptr;
    memcpy(dest, captureBuf + captureRead, count1);
    memcpy(dest+count1, captureBuf, count2);
    captureRead = ptr;
    return count1 + count2;
  }
}

//dump bytes into capture buffer. 0 if successful, 1 if not enough room
int ringWrite(char* src, int count){
  int wall = captureRead;
  int population = captureWrite >= wall
    ? captureWrite - wall
    : captureWrite + (CAPTURE_SIZE - wall);
  int count1;
  int count2;
  if(population + count > CAPTURE_SIZE - 1){ // not enough room
    return 1;
  }
  else if(captureWrite + count < CAPTURE_SIZE){ // normal copy
    memcpy(captureBuf + captureWrite, src, count);
    captureWrite += count;
    return 0;
  }
  else{ // need to copy twice to account for wrap
    count1 = CAPTURE_SIZE - captureWrite;
    count2 = count - count1;
    memcpy(captureBuf + captureWrite, src, count1);
    if(count2 > 0) memcpy(captureBuf, src + count1, count2);
    captureWrite = (captureWrite + count) % CAPTURE_SIZE;
    return 0;
  }
}
*/

void midiNotification(const MIDINotification* message, void* refCon){
  fprintf(stderr, "midiNotification\n");
}

void captureWorker(const MIDIPacketList* packetList, void* refCon, void* srcConn){
  uint64_t now = mach_absolute_time();
  const MIDIPacket* packet = &packetList->packet[0];
  int i;
  fprintf(stderr, "captureWorker (%d packets)\n", packetList->numPackets);
  for(i=0; i<packetList->numPackets; i++){
    printf("%llu %llu %d: %x %d %d\n",
      now,
      packet->timeStamp,
      packet->length,
      packet->data[0],
      packet->data[1],
      packet->data[2]
    );
    packet = MIDIPacketNext(packet);
  }
}

int setupCoreMidi(){
  OSStatus status;

  /* creating a client */
  status = MIDIClientCreate(
    CFStringCreateWithCString(NULL, "My Midi Client", kCFStringEncodingASCII),
    midiNotification, NULL, &client
  );
  if(status != noErr){
    fprintf(stderr, "core midi client create error %d\n", status);
    exit(-1);
  }

  status = MIDIDestinationCreate(
    client,
    CFStringCreateWithCString(NULL,"Epichord Capture",kCFStringEncodingASCII),
    captureWorker,
    NULL,
    &inputPort
  );
  if(status != noErr){
    fprintf(stderr, "** SOUND error creating CoreMidi destination %d\n", status);
    exit(-1);
  }

  /* TO ADVERTISE AN INPUT PORT... CREATE A DESTINATION ENDPOINT */
  /* TO ADVERTISE AN OUTPUT PORT... CREATE A SOURCE ENDPOINT */
  /* TO SEND TO WHATEVER CONNECTED TO OUR OUTPUT USE MIDIRECEIVED */
  /* MIDI THAT COMES IN ON OUR OUTPUT TRIGGERS THE CAPTURE CALLBACK */

  status = MIDISourceCreate(
    client,
    CFStringCreateWithCString(NULL,"Epichord Output",kCFStringEncodingASCII),
    &outputPort
  );
  if(status != noErr){
    fprintf(stderr, "** SOUND error creating CoreMidi source %d\n", status);
    exit(-1);
  }

  return 0;
}

struct tempoChange* loadTempoChangeData(FILE* tempoFile, int* count){
  int tempoMax = 32;
  int tempoPtr = 0;
  int bytesRead;
  unsigned char seven[7];
  struct tempoChange* tempoBuf = malloc(tempoMax * sizeof(struct tempoChange));
  if(tempoBuf == NULL){
    fprintf(stderr, "** SOUND malloc of tempoBuf failed\n");
    exit(-1);
  }
  for(;;){
    bytesRead = fread(seven, 1, 7, tempoFile);
    if(bytesRead == 0) break;
    if(bytesRead != 7) {
      fprintf(stderr, "** SOUND tempo data file ends with %d bytes not 7\n", bytesRead);
      exit(-1);
    }
    tempoBuf[tempoPtr].tick = seven[0]<<24 | seven[1]<<16 | seven[2]<<8 | seven[3];
    tempoBuf[tempoPtr].uspq = seven[4]<<16 | seven[5]<<8  | seven[6];
    tempoPtr++;
    if(tempoPtr == tempoMax){
      tempoBuf = realloc(tempoBuf, tempoMax*2*sizeof(struct tempoChange));
      if(tempoBuf == NULL){
        fprintf(stderr, "** SOUND failed to realloc tempo buffer\n");
        exit(-1);
      }
      tempoMax *= 2;
    }
  }
  *count = tempoPtr;
  return tempoBuf;
}

struct sequencerEvent* loadSequenceData(FILE* sequenceFile, int* count){
  int eventMax = 256;
  int eventPtr = 0;
  int bytesRead;
  unsigned char seven[7];
  struct sequencerEvent* eventBuf = malloc(eventMax * sizeof(struct sequencerEvent));
  if(eventBuf == NULL){
    fprintf(stderr, "** SOUND malloc of eventBuf failed\n");
    exit(-1);
  }
  for(;;){
    bytesRead = fread(seven, 1, 7, sequenceFile);
    if(bytesRead == 0) break;
    if(bytesRead != 7) {
      fprintf(stderr,
        "** SOUND sequence data file ends with %d bytes not 7\n", bytesRead);
      exit(-1);
    }
    eventBuf[eventPtr].tick = seven[0]<<24 | seven[1]<<16 | seven[2]<<8 | seven[3];
    eventBuf[eventPtr].typeChan = seven[4];
    eventBuf[eventPtr].arg1 = seven[5];
    eventBuf[eventPtr].arg2 = seven[6];
    eventPtr++;
    if(eventPtr == eventMax){
      eventBuf = realloc(eventBuf, eventMax*2*sizeof(struct sequencerEvent));
      if(eventBuf == NULL){
        fprintf(stderr, "** SOUND failed to realloc event buffer\n");
        exit(-1);
      }
      eventMax *= 2;
    }
  }
  *count = eventPtr;
  return eventBuf;
}


void recomputeEventTimes(
  struct sequencerEvent* events,
  int eventCount,
  struct tempoChange* tempoChanges,
  int tempoCount,
  uint32_t ticksPerBeat
){
  int i, j;
  uint32_t default_uspq = DEFAULT_USPQ;
  uint32_t uspq = default_uspq;
  uint64_t prevNs = 0;
  uint32_t prevTick = 0;
  uint32_t deltaTicks;
  for(i=0, j=0; i < tempoCount; i++){
    deltaTicks = tempoChanges[i].tick - prevTick;
    tempoChanges[i].atNs = prevNs + deltaTicks*1000.0*uspq / ticksPerBeat;

    for(;;){
      if(j >= eventCount) break;
      deltaTicks = events[j].tick - prevTick;
      events[j].atNs = prevNs + deltaTicks*1000.0*uspq / ticksPerBeat;
      if(events[j].atNs <= tempoChanges[i].atNs) j++;
      else break;
    }

    prevTick = tempoChanges[i].tick;
    prevNs = tempoChanges[i].atNs;
    uspq = tempoChanges[i].uspq;
  }

  for(;;){
    if(j >= eventCount) break;
    deltaTicks = events[j].tick - prevTick;
    events[j].atNs = prevNs + deltaTicks*1000.0*uspq / ticksPerBeat;
    j++;
  }

}

int prefix(const char *pre, const char *str)
{
  return strncmp(pre, str, strlen(pre)) == 0;
}

// load raw sequence and tempo data from two files, then delete the files
struct sequence* loadData(char* sequencePath, char* tempoPath){
  FILE* tempoFile;
  FILE* sequenceFile;
  struct sequencerEvent* events;
  struct tempoChange* tempoChanges;
  int eventCount;
  int tempoChangeCount;
  struct sequence* seq;

  if(!prefix("/tmp/epichord-", sequencePath)){
    fprintf(stderr, "** refuse to load file from this location (%s)\n", sequencePath);
    exit(-1);
  }
  if(!prefix("/tmp/epichord-", tempoPath)){
    fprintf(stderr, "** refuse to load file from this location (%s)\n", tempoPath);
    exit(-1);
  }

  tempoFile = fopen(tempoPath, "r");
  if(tempoFile == NULL){
    fprintf(stderr,
      "SOUND failed to open tempo file: %s %s\n", tempoPath, strerror(errno));
    exit(-1);
  }
  tempoChanges = loadTempoChangeData(tempoFile, &tempoChangeCount);
  fclose(tempoFile);

  sequenceFile = fopen(sequencePath, "r");
  if(sequenceFile == NULL){
    fprintf(stderr,
      "SOUND failed to open sequence file: %s %s\n", sequencePath, strerror(errno));
    exit(-1);
  }
  events = loadSequenceData(sequenceFile, &eventCount);
  fclose(sequenceFile);

  recomputeEventTimes(events, eventCount, tempoChanges, tempoChangeCount, ticksPerBeat);

  seq = malloc(sizeof(struct sequence));
  if(seq == NULL){
    fprintf(stderr, "** SOUND failed to malloc sequence\n");
    exit(-3);
  }
  seq->eventCount = eventCount;
  seq->tempoChangeCount = tempoChangeCount;
  seq->events = events;
  seq->tempoChanges = tempoChanges;
/*
  if(unlink(sequencePath)){
    fprintf(stderr, "** SOUND failed to remove dump file (%s)\n", strerror(errno));
    exit(-1);
  }
  if(unlink(tempoPath)){
    fprintf(stderr, "** SOUND failed to remove dump file (%s)\n", strerror(errno));
    exit(-1);
  }
  
*/
  return seq;
}


// execute midi events within the range fromNs to toNs where 0 is the start
// of the song. should consider loop position to repeat parts indefinitely.
void dispatchFrame(struct sequence* seq, uint64_t fromNs, uint64_t toNs){
  //fprintf(stderr, "[%llu, %llu)\n", fromNs, toNs);
  unsigned char packetListStorage[PACKET_LIST_SIZE];
  MIDIPacketList* packetList = (MIDIPacketList*) packetListStorage;
  MIDIPacket* packet;
  int packetSize = sizeof(uint64_t) + sizeof(uint16_t) + 3;
  unsigned char midi[3];
  int i, j;
  int midiSize;
  int c = 0;

  int eventCount = seq->eventCount;
  struct sequencerEvent* events = seq->events;

  for(i=0; i<eventCount; i++){ // get to the first event in range
    if(events[i].atNs >= fromNs) break;
  }
  //printf("i = %d\n", i);

  packet = MIDIPacketListInit(packetList);

  for(j=0; i<eventCount; i++, j++){ // for each event in range
    if(events[i].atNs >= toNs) break;
    //printf("i=%d j=%d %u %x %u %u\n", i, j, events[i].tick, events[i].typeChan, events[i].arg1, events[i].arg2);

    c++;
    midi[0] = events[i].typeChan;
    midi[1] = events[i].arg1;
    midi[2] = events[i].arg2;
    midiSize = 3;
    if((midi[0] & 0xf0) == 0xc0 || (midi[0] & 0xf0) == 0xd0) midiSize = 2;
    if((midi[0] & 0xf0) != 0x80){
      //fprintf(stderr, "%llu %02x %02x %02x\n", events[i].atNs, midi[0], midi[1], midi[2]);
    }
    if((midi[0] & 0xf0) == 0x90 && midi[2] > 0){
      rememberNoteOn(midi[0] & 0x0f, midi[1]);
    }
    if((midi[0] & 0xf0) == 0x80 || ((midi[0] & 0xf0) == 0x90 && midi[2] == 0)){
      forgetNoteOn(midi[0] & 0x0f, midi[1]);
    }
    packet = MIDIPacketListAdd(
      packetList,
      PACKET_LIST_SIZE,
      packet, 
      events[i].atNs + absoluteSongStartNs,
      midiSize,
      midi
    );
    if(packet == NULL){
      fprintf(stderr, "** SOUND unable to MIDIPacketListAdd\n");
      exit(-1);
    }
    //printf("still ok\n");
  }

  //printf("output\n");
  MIDIReceived(outputPort, packetList);
  //printf("hmm\n");
}

// a frame is a 20ms chunk of time. we play 20ms ahead of time, sleep for
// 20ms, play 20ms of events, and sleep for ~20ms depending on overshot.
// ASSUMPTION mach_absolute_time returns nanoseconds, that is num=denom=1
void* sleepWakeAndDispatchFrame(){
  uint64_t sleepTargetNs;
  uint64_t currentNs;
  uint64_t overshot;
  struct sequence* sequenceSnap;
  struct sequence* sequenceSnapPrev = NULL;

  currentNs = mach_absolute_time();
  absolutePlayHeadNs = (currentNs-currentNs%FRAME_SIZE_NS) + FRAME_SIZE_NS;
  absoluteLeadingEdgeNs = absolutePlayHeadNs + FRAME_SIZE_NS;
  absoluteSongStartNs = absolutePlayHeadNs - songNs;

  for(;;){
    sequenceSnap = currentSequence;
    if(sequenceSnapPrev && sequenceSnapPrev != sequenceSnap){
      trashSequence(sequenceSnapPrev);
    }
    sequenceSnapPrev = sequenceSnap;

    if(playFlag == 0){
      onlineSeekFlag = 0;
      killAll();
      return NULL;
    }
    if(onlineSeekFlag == 1){
      killAll();
      songNs = onlineSeekTargetNs;
      absolutePlayHeadNs = currentNs;
      absoluteLeadingEdgeNs = absolutePlayHeadNs + FRAME_SIZE_NS;
      absoluteSongStartNs = absolutePlayHeadNs - songNs;
      onlineSeekFlag = 0;
    }
    if(cutAllFlag){
      killAll();
      cutAllFlag = 0;
    }
    if(loopFlag && songNs > loopEndNs){
      killAll();
      songNs = loopStartNs;
      absolutePlayHeadNs = currentNs;
      absoluteLeadingEdgeNs = absolutePlayHeadNs + FRAME_SIZE_NS;
      absoluteSongStartNs = absolutePlayHeadNs - songNs;
    }

    if(loopFlag && songNs + FRAME_SIZE_NS > loopEndNs){
      overshot = songNs + FRAME_SIZE_NS - loopEndNs;
      dispatchFrame(sequenceSnap, songNs, loopEndNs + 1); 
      /*fprintf(stderr, "dispatch frame [%llu, %llu] [%llu, %llu]\n",
        songNs, loopEndNs,
        absolutePlayHeadNs, absoluteLeadingEdgeNs
      );*/
      absolutePlayHeadNs += loopEndNs - songNs;
      absoluteSongStartNs = absolutePlayHeadNs - loopStartNs;
      dispatchFrame(sequenceSnap, loopStartNs, loopStartNs + overshot);
      /*fprintf(stderr, "dispatch frame [%llu, %llu) [%llu, %llu)\n",
        loopStartNs, loopStartNs + overshot,
        absolutePlayHeadNs, absoluteLeadingEdgeNs
      );*/
      songNs = loopStartNs + overshot;
    }
    else{
      dispatchFrame(sequenceSnap, songNs, songNs + FRAME_SIZE_NS);
      /*fprintf(stderr, "dispatch frame [%llu, %llu) [%llu, %llu)\n",
        songNs, songNs+FRAME_SIZE_NS,
        absolutePlayHeadNs, absoluteLeadingEdgeNs
      );*/
      songNs += FRAME_SIZE_NS; // looping wrapping...
    }
    sleepTargetNs = absolutePlayHeadNs - currentNs;
    absolutePlayHeadNs += FRAME_SIZE_NS;
    absoluteLeadingEdgeNs += FRAME_SIZE_NS;
    usleep(sleepTargetNs / 1000);
    currentNs = mach_absolute_time();
    if(currentNs > absolutePlayHeadNs){ // over sleep
      fprintf(stderr, "SOUND over slept! game over man!\n");
      exit(-1);
    }
  }
}


void spawnDispatchThread(){
  int ret;
  ret = pthread_create(&dispatchThread, NULL, sleepWakeAndDispatchFrame, NULL);
  if(ret){
    fprintf(stderr,"SOUND dispatch thread failed to create: %s\n",strerror(errno));
    exit(-1);
  }
}

void joinDispatchThread(){
  int ret;
  ret = pthread_join(dispatchThread, NULL);
  if(ret){
    fprintf(
      stderr,
      "SOUND dispatch thread failed to join: %s (%d)\n",
      strerror(ret),
      ret
    );
    exit(-1);
  }
}

void interrupt(int unused){
  fprintf(stderr, "** SOUND interrupted by signal\n");
  if(playFlag == 1){
    playFlag = 0;
    joinDispatchThread();
    usleep(100000);
  }
  exit(0);
}


void* garbageWorker(){
  int i;
  for(;;){
    pthread_cond_wait(&garbageSignal, &garbageMutex);
    for(i=0; i<GARBAGE_SIZE; i++){
      if(garbage[i] != NULL){
        free(garbage[i]->events);
        free(garbage[i]->tempoChanges);
        free(garbage[i]);
        garbage[i] = NULL;
      }
    }
  }
}

void spawnGarbageThread(){
  pthread_t unused;
  pthread_mutex_init(&garbageMutex, NULL);
  pthread_cond_init(&garbageSignal, NULL);
  pthread_create(&unused, NULL, garbageWorker, NULL);
}

void initGarbage(){
  int i;
  for(i=0; i<GARBAGE_SIZE; i++){
    garbage[i] = NULL;
  }
}

void initNullSequence(){
  struct sequence* seq = malloc(sizeof(struct sequence));
  seq->eventCount = 0;
  seq->tempoChangeCount = 0;
  seq->events = NULL;
  seq->tempoChanges = NULL;
  currentSequence = seq;
}


void executeMidi(int type, int channel, int arg1, int arg2){
  uint64_t now = mach_absolute_time();
  unsigned char packetListStorage[50];
  MIDIPacketList* packetList = (MIDIPacketList*) packetListStorage;
  MIDIPacket* packet;
  unsigned char midi[3];
  int midiSize;
  if(playFlag == 1) return;
  packet = MIDIPacketListInit(packetList);
  midi[0] = type << 4 | channel;
  midi[1] = arg1;
  midi[2] = arg2;
  midiSize = 3;
  if((midi[0] & 0xf0) == 0xc0 || (midi[0] & 0xf0) == 0xd0) midiSize = 2;
  if((midi[0] & 0xf0) != 0x80){
    //fprintf(stderr, "%llu %02x %02x %02x\n", now, midi[0], midi[1], midi[2]);
  }
  if((midi[0] & 0xf0) == 0x90 && midi[2] > 0){
    rememberNoteOn(midi[0] & 0x0f, midi[1]);
  }
  if((midi[0] & 0xf0) == 0x80 || ((midi[0] & 0xf0) == 0x90 && midi[2] == 0)){
    forgetNoteOn(midi[0] & 0x0f, midi[1]);
  }
  packet = MIDIPacketListAdd(
    packetList,
    50,
    packet, 
    now,
    midiSize,
    midi
  );
  if(packet == NULL){
    fprintf(stderr, "** SOUND 'execute' unable to MIDIPacketListAdd\n");
    exit(-1);
  }

  MIDIReceived(outputPort, packetList);
}

void stdinWorker(){
  char buf[INBUF_SIZE];
  char command[INBUF_SIZE];
  char arg1[INBUF_SIZE];
  char arg2[INBUF_SIZE];
  int number;
  int numerator;
  int denominator;
  int result;
  double loop0;
  double loop1;
  int midi[4];

  fgets(buf, INBUF_SIZE, stdin);
  if(ferror(stdin)){
    fprintf(stderr, "SOUND error while reading stdin. <%s>\n", strerror(errno));
    exit(-1);
  }
  if(feof(stdin)){
    fprintf(stderr, "SOUND stdin is EOF. Terminating\n");
    exit(0);
  }

  buf[strlen(buf)-1] = 0;

  result = sscanf(buf, "%s", command);
  if(result <= 0){
    fprintf(stderr, "SOUND unrecognized command\n");
  }
  else if(strcmp(command, "load")==0){
    result = sscanf(buf, "%s %s %s", command, arg1, arg2);
    if(result < 3){
      fprintf(stderr, "** SOUND invalid LOAD command (%s)\n", buf);
      exit(-1);
    }
    currentSequence = loadData(arg1, arg2);
    emptyTrash();
  }
  else if(strcmp(command, "play")==0){
    if(playFlag == 0){
      playFlag = 1;
      spawnDispatchThread();
    }
    else{
      fprintf(stderr, "SOUND refusing to play, playFlag=%d\n", playFlag);
    }
  }
  else if(strcmp(command, "stop")==0){
    if(playFlag == 1){
      playFlag = 0;
      joinDispatchThread();
    }
    else{
      fprintf(stderr, "SOUND stop ignored, playFlag=%d\n", playFlag);
    }
  }
  else if(strcmp(command, "seek")==0){
    result = sscanf(buf, "%s %d %d/%d", command, &number, &numerator, &denominator);
    if(result < 4){
      result = sscanf(buf, "%s %d", command, &number);
      if(result < 2){
        fprintf(stderr, "** SOUND invalid SEEK command (%s)\n", buf);
        return;
      }
      numerator = 0;
      denominator = 1;
    }
    executeSeek(number, numerator, denominator);
  }
  else if(strcmp(command, "crash")==0){
    abort();
  }
  else if(strcmp(command, "exit")==0){
    interrupt(0);
  }
  else if(strcmp(command, "cut-all")==0){
    if(playFlag == 0){
      killAll();
    }
    else{
      cutAllFlag = 1;
      usleep(FRAME_SIZE_NS/1000);
    }
  }
  else if(strcmp(command, "set-loop")==0){
    result = sscanf(buf, "%s %lf %lf", command, &loop0, &loop1);
    if(result < 3){
      fprintf(stderr, "** SOUND invalid SET_LOOP command (%s)\n", buf);
    }
    else{
      setLoopEndpoints(loop0, loop1);
    }
  }
  else if(strcmp(command, "enable-loop")==0){
    if(loopInitialized == 0){
      fprintf(stderr, "** SOUND can't enable loop, not initialized\n");
    }
    else{
      loopFlag = 1;
    }
  }
  else if(strcmp(command, "disable-loop")==0){
    loopFlag = 0;
  }
  else if(strcmp(command, "ticks-per-beat")==0){
    result = sscanf(buf, "%s %d", command, &number);
    if(result < 2){
      fprintf(stderr, "** SOUND invalid TICKS_PER_BEAT command (%s)\n", buf);
    }
    if(number <= 0){
      fprintf(stderr, "** SOUND ignoring setting ticks per beat to %d\n", number);
    }
    else{
      if(playFlag){
        fprintf(stderr, "SOUND not changing ticks per beat while playing\n");
      }
      else{
        ticksPerBeat = number;
      }
    }
  }
  else if(strcmp(command, "tell")==0){
    fprintf(stdout, "%lf\n", getCurrentBeat());
    fflush(stdout);
  }
  else if(strcmp(command, "execute")==0){
    result = sscanf(
      buf, "%s %d %d %d %d",
      command,
      &midi[0], &midi[1], &midi[2], &midi[3]
    );
    if(result < 5){
      fprintf(stderr, "** SOUND invalid EXECUTE command (%s)\n", buf);
    }
    else{
      executeMidi(midi[0], midi[1], midi[2], midi[3]);
    }
  }
  else if(strcmp(command, "enable-capture")==0){
  }
  else if(strcmp(command, "disable-capture")==0){
  }
  else if(strcmp(command, "capture")==0){
  }
  else{
    fprintf(stderr, "SOUND unrecognized command (%s)\n", buf);
  }
}

int main(int argc, char* argv[]){
  fprintf(stderr, "SOUND Hello World\n");
  
  if(setupCoreMidi()){
    fprintf(stderr, "SOUND CoreMidi setup failed.\n");
    exit(-1);
  }

  initNullSequence();
  initPlayingNotes();
  initGarbage();
  spawnGarbageThread();

  signal(SIGINT, interrupt);

  for(;;) stdinWorker();
  fprintf(stderr, "SOUND impossible -2\n");
  return -2;
}

