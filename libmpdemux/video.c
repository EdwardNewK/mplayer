// read video frame

#include "config.h"

#include <stdio.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mp_msg.h"
#include "help_mp.h"

#include "stream.h"
#include "demuxer.h"
#include "stheader.h"
#include "parse_es.h"
#include "mpeg_hdr.h"

/* sub_cc (closed captions)*/
#include "sub_cc.h"

#ifdef USE_LIBAVCODEC_SO
#include <ffmpeg/avcodec.h>
#elif defined(USE_LIBAVCODEC)
#include "libavcodec/avcodec.h"
#else
#define FF_INPUT_BUFFER_PADDING_SIZE 8
#endif

/* biCompression constant */
#define BI_RGB        0L

#ifdef STREAMING_LIVE555
#include "demux_rtp.h"
#endif

static mp_mpeg_header_t picture;

static int telecine=0;
static float telecine_cnt=-2.5;

int video_read_properties(sh_video_t *sh_video){
demux_stream_t *d_video=sh_video->ds;

enum {
	VIDEO_MPEG12,
	VIDEO_MPEG4,
	VIDEO_H264,
	VIDEO_OTHER
} video_codec;

if((d_video->demuxer->file_format == DEMUXER_TYPE_PVA) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_ES) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_GXF) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_PES) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_PS && ((! sh_video->format) || (sh_video->format==0x10000001) || (sh_video->format==0x10000002))) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_TY) ||
   (d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_TS && ((sh_video->format==0x10000001) || (sh_video->format==0x10000002)))
#ifdef STREAMING_LIVE555
  || ((d_video->demuxer->file_format == DEMUXER_TYPE_RTP) && demux_is_mpeg_rtp_stream(d_video->demuxer))
#endif
  )
    video_codec = VIDEO_MPEG12;
  else if((d_video->demuxer->file_format == DEMUXER_TYPE_MPEG4_ES) ||
    ((d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_TS) && (sh_video->format==0x10000004)) ||
    ((d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_PS) && (sh_video->format==0x10000004))
  )
    video_codec = VIDEO_MPEG4;
  else if((d_video->demuxer->file_format == DEMUXER_TYPE_H264_ES) ||
    ((d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_TS) && (sh_video->format==0x10000005)) ||
    ((d_video->demuxer->file_format == DEMUXER_TYPE_MPEG_PS) && (sh_video->format==0x10000005))
  )
    video_codec = VIDEO_H264;
  else
    video_codec = VIDEO_OTHER;
    
// Determine image properties:
switch(video_codec){
 case VIDEO_OTHER: {
 if((d_video->demuxer->file_format == DEMUXER_TYPE_ASF) || (d_video->demuxer->file_format == DEMUXER_TYPE_AVI)) {
  // display info: 
  
#if 0
    if(sh_video->bih->biCompression == BI_RGB &&
       (sh_video->video.fccHandler == mmioFOURCC('D', 'I', 'B', ' ') ||
        sh_video->video.fccHandler == mmioFOURCC('R', 'G', 'B', ' ') ||
        sh_video->video.fccHandler == mmioFOURCC('R', 'A', 'W', ' ') ||
        sh_video->video.fccHandler == 0)) {
                sh_video->format = mmioFOURCC(0, 'R', 'G', 'B') | sh_video->bih->biBitCount;
    }
    else 					    
#endif
        sh_video->format=sh_video->bih->biCompression;

    sh_video->disp_w=sh_video->bih->biWidth;
    sh_video->disp_h=abs(sh_video->bih->biHeight);

#if 0
    /* hack to support decoding of mpeg1 chunks in AVI's with libmpeg2 -- 2002 alex */
    if ((sh_video->format == 0x10000001) ||
	(sh_video->format == 0x10000002) ||
	(sh_video->format == mmioFOURCC('m','p','g','1')) ||
	(sh_video->format == mmioFOURCC('M','P','G','1')) ||
	(sh_video->format == mmioFOURCC('m','p','g','2')) ||
	(sh_video->format == mmioFOURCC('M','P','G','2')) ||
	(sh_video->format == mmioFOURCC('m','p','e','g')) ||
	(sh_video->format == mmioFOURCC('M','P','E','G')))
    {
	int saved_pos, saved_type;

	/* demuxer pos saving is required for libavcodec mpeg decoder as it's
	   reading the mpeg header self! */
	
	saved_pos = d_video->buffer_pos;
	saved_type = d_video->demuxer->file_format;

	d_video->demuxer->file_format = DEMUXER_TYPE_MPEG_ES;
	video_read_properties(sh_video);
	d_video->demuxer->file_format = saved_type;
	d_video->buffer_pos = saved_pos;
//	goto mpeg_header_parser;
    }
#endif
  }
  break;
 }
 case VIDEO_MPEG4: {
   int pos = 0, vop_cnt=0, units[3];
   videobuf_len=0; videobuf_code_len=0;
   mp_msg(MSGT_DECVIDEO,MSGL_V,"Searching for Video Object Start code... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      if(i<=0x11F) break; // found it!
      if(!i || !skip_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\n");
   if(!videobuffer) {
     videobuffer=(char*)memalign(8,VIDEOBUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
     if (videobuffer) memset(videobuffer+VIDEOBUFFER_SIZE, 0, FF_INPUT_BUFFER_PADDING_SIZE);
     else {
       mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_ShMemAllocFail);
       return 0;
     }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"Searching for Video Object Layer Start code... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      mp_msg(MSGT_DECVIDEO,MSGL_V,"M4V: 0x%X\n",i);
      if(i>=0x120 && i<=0x12F) break; // found it!
      if(!i || !read_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   pos = videobuf_len+4;
   if(!read_video_packet(d_video)){ 
     mp_msg(MSGT_DECVIDEO,MSGL_ERR,"Can't read Video Object Layer Header\n");
     return 0;
   }
   mp4_header_process_vol(&picture, &(videobuffer[pos]));
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK! FPS SEEMS TO BE %.3f\nSearching for Video Object Plane Start code... ", sh_video->fps);fflush(stdout);
 mp4_init: 
   while(1){
      int i=sync_video_packet(d_video);
      if(i==0x1B6) break; // found it!
      if(!i || !read_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   pos = videobuf_len+4;
   if(!read_video_packet(d_video)){ 
     mp_msg(MSGT_DECVIDEO,MSGL_ERR,"Can't read Video Object Plane Header\n");
     return 0;
   }
   mp4_header_process_vop(&picture, &(videobuffer[pos]));
   units[vop_cnt] = picture.timeinc_unit;
   vop_cnt++;
   //mp_msg(MSGT_DECVIDEO,MSGL_V, "TYPE: %d, unit: %d\n", picture.picture_type, picture.timeinc_unit);
   if(!picture.fps) {
     int i, mn, md, mx, diff;
     if(vop_cnt < 3)
          goto mp4_init;

     i=0;
     mn = mx = units[0];  
     for(i=0; i<3; i++) {
       if(units[i] < mn)
         mn = units[i];
       if(units[i] > mx)
         mx = units[i];
     }
     md = mn;
     for(i=0; i<3; i++) {
       if((units[i] > mn) && (units[i] < mx))
         md = units[i];
     }
     mp_msg(MSGT_DECVIDEO,MSGL_V, "MIN: %d, mid: %d, max: %d\n", mn, md, mx);
     if(mx - md > md - mn)
       diff = md - mn;
     else
       diff = mx - md;
     if(diff > 0){
       picture.fps = ((float)picture.timeinc_resolution) / diff;
       mp_msg(MSGT_DECVIDEO,MSGL_V, "FPS seems to be: %f, resolution: %d, delta_units: %d\n", picture.fps, picture.timeinc_resolution, diff);
     }
   }
   if(picture.fps) {
    sh_video->fps=picture.fps;
    sh_video->frametime=1.0/picture.fps;
    mp_msg(MSGT_DECVIDEO,MSGL_INFO, "FPS seems to be: %f\n", picture.fps);
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\n");
   sh_video->format=0x10000004;
   break;
 }
 case VIDEO_H264: {
   int pos = 0;
   videobuf_len=0; videobuf_code_len=0;
   mp_msg(MSGT_DECVIDEO,MSGL_V,"Searching for sequence parameter set... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      if((i&~0x60) == 0x107 && i != 0x107) break; // found it!
      if(!i || !skip_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\n");
   if(!videobuffer) {
     videobuffer=(char*)memalign(8,VIDEOBUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
     if (videobuffer) memset(videobuffer+VIDEOBUFFER_SIZE, 0, FF_INPUT_BUFFER_PADDING_SIZE);
     else {
       mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_ShMemAllocFail);
       return 0;
     }
   }
   pos = videobuf_len+4;
   if(!read_video_packet(d_video)){ 
     mp_msg(MSGT_DECVIDEO,MSGL_ERR,"Can't read sequence parameter set\n");
     return 0;
   }
   h264_parse_sps(&picture, &(videobuffer[pos]), videobuf_len - pos);
   mp_msg(MSGT_DECVIDEO,MSGL_V,"Searching for picture parameter set... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      mp_msg(MSGT_DECVIDEO,MSGL_V,"H264: 0x%X\n",i);
      if((i&~0x60) == 0x108 && i != 0x108) break; // found it!
      if(!i || !read_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\nSearching for Slice... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      if((i&~0x60) == 0x101 || (i&~0x60) == 0x102 || (i&~0x60) == 0x105) break; // found it!
      if(!i || !read_video_packet(d_video)){
        mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
	return 0;
      }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\n");
   sh_video->format=0x10000005;
   if(picture.fps) {
     sh_video->fps=picture.fps;
     sh_video->frametime=1.0/picture.fps;
     mp_msg(MSGT_DECVIDEO,MSGL_INFO, "FPS seems to be: %f\n", picture.fps);
   }
   break;
 }
 case VIDEO_MPEG12: {
mpeg_header_parser:
   // Find sequence_header first:
   videobuf_len=0; videobuf_code_len=0;
   telecine=0; telecine_cnt=-2.5;
   mp_msg(MSGT_DECVIDEO,MSGL_V,"Searching for sequence header... ");fflush(stdout);
   while(1){
      int i=sync_video_packet(d_video);
      if(i==0x1B3) break; // found it!
      if(!i || !skip_video_packet(d_video)){
        if( mp_msg_test(MSGT_DECVIDEO,MSGL_V) )  mp_msg(MSGT_DECVIDEO,MSGL_V,"NONE :(\n");
        mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_MpegNoSequHdr);
	return 0;
      }
   }
   mp_msg(MSGT_DECVIDEO,MSGL_V,"OK!\n");
//   sh_video=d_video->sh;sh_video->ds=d_video;
//   mpeg2_init();
   // ========= Read & process sequence header & extension ============
   if(!videobuffer) {
     videobuffer=(char*)memalign(8,VIDEOBUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
     if (videobuffer) memset(videobuffer+VIDEOBUFFER_SIZE, 0, FF_INPUT_BUFFER_PADDING_SIZE);
     else {
       mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_ShMemAllocFail);
       return 0;
     }
   }
   
   if(!read_video_packet(d_video)){ 
     mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_CannotReadMpegSequHdr);
     return 0;
   }
   if(mp_header_process_sequence_header (&picture, &videobuffer[4])) {
     mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_BadMpegSequHdr); 
     goto mpeg_header_parser;
     //return 0;
   }
   if(sync_video_packet(d_video)==0x1B5){ // next packet is seq. ext.
//    videobuf_len=0;
    int pos=videobuf_len;
    if(!read_video_packet(d_video)){ 
      mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_CannotReadMpegSequHdrEx);
      return 0;
    }
    if(mp_header_process_extension (&picture, &videobuffer[pos+4])) {
      mp_msg(MSGT_DECVIDEO,MSGL_ERR,MSGTR_BadMpegSequHdrEx);
      return 0;
    }
   }
   
//   printf("picture.fps=%d\n",picture.fps);
   // display info:
   sh_video->format=picture.mpeg1?0x10000001:0x10000002; // mpeg video
   sh_video->fps=picture.fps;
   if(!sh_video->fps){
//     if(!force_fps){
//       fprintf(stderr,"FPS not specified (or invalid) in the header! Use the -fps option!\n");
//       return 0;
//     }
     sh_video->frametime=0;
   } else {
     sh_video->frametime=1.0/picture.fps;
   }
   sh_video->disp_w=picture.display_picture_width;
   sh_video->disp_h=picture.display_picture_height;
   // bitrate:
   if(picture.bitrate!=0x3FFFF) // unspecified/VBR ?
       sh_video->i_bps=picture.bitrate * 400 / 8;
   // info:
   mp_dbg(MSGT_DECVIDEO,MSGL_DBG2,"mpeg bitrate: %d (%X)\n",picture.bitrate,picture.bitrate);
   mp_msg(MSGT_DECVIDEO,MSGL_INFO,"VIDEO:  %s  %dx%d  (aspect %d)  %5.3f fps  %5.1f kbps (%4.1f kbyte/s)\n",
    picture.mpeg1?"MPEG1":"MPEG2",
    sh_video->disp_w,sh_video->disp_h,
    picture.aspect_ratio_information,
    sh_video->fps,
    sh_video->i_bps * 8 / 1000.0,
    sh_video->i_bps / 1000.0 );
  break;
 }
} // switch(file_format)

return 1;
}

void ty_processuserdata( unsigned char* buf, int len );

static void process_userdata(unsigned char* buf,int len){
    int i;
    /* if the user data starts with "CC", assume it is a CC info packet */
    if(len>2 && buf[0]=='C' && buf[1]=='C'){
//    	mp_msg(MSGT_DECVIDEO,MSGL_DBG2,"video.c: process_userdata() detected Closed Captions!\n");
	if(subcc_enabled) subcc_process_data(buf+2,len-2);
    }
    if( len > 2 && buf[ 0 ] == 'T' && buf[ 1 ] == 'Y' )
    {
       ty_processuserdata( buf + 2, len - 2 );
       return;
    }
    if(verbose<2) return;
    printf( "user_data: len=%3d  %02X %02X %02X %02X '",
	    len, buf[0], buf[1], buf[2], buf[3]);
    for(i=0;i<len;i++)
//	if(buf[i]>=32 && buf[i]<127) putchar(buf[i]);
	if(buf[i]&0x60) putchar(buf[i]&0x7F);
    printf("'\n");
}

int video_read_frame(sh_video_t* sh_video,float* frame_time_ptr,unsigned char** start,int force_fps){
    demux_stream_t *d_video=sh_video->ds;
    demuxer_t *demuxer=d_video->demuxer;
    float frame_time=1;
    float pts1=d_video->pts;
    float pts=0;
    int picture_coding_type=0;
//    unsigned char* start=NULL;
    int in_size=0;
    
    *start=NULL;

  if(demuxer->file_format==DEMUXER_TYPE_MPEG_ES || 
     demuxer->file_format==DEMUXER_TYPE_MPEG_GXF ||
     demuxer->file_format==DEMUXER_TYPE_MPEG_PES ||
  	(demuxer->file_format==DEMUXER_TYPE_MPEG_PS && ((! sh_video->format) || (sh_video->format==0x10000001) || (sh_video->format==0x10000002)))
		  || demuxer->file_format==DEMUXER_TYPE_PVA || 
		  ((demuxer->file_format==DEMUXER_TYPE_MPEG_TS) && ((sh_video->format==0x10000001) || (sh_video->format==0x10000002)))
		  || demuxer->file_format==DEMUXER_TYPE_MPEG_TY
#ifdef STREAMING_LIVE555
    || (demuxer->file_format==DEMUXER_TYPE_RTP && demux_is_mpeg_rtp_stream(demuxer))
#endif
  ){
        int in_frame=0;
        //float newfps;
        //videobuf_len=0;
        while(videobuf_len<VIDEOBUFFER_SIZE-MAX_VIDEO_PACKET_SIZE){
          int i=sync_video_packet(d_video);
	  //void* buffer=&videobuffer[videobuf_len+4];
	  int start=videobuf_len+4;
          if(in_frame){
            if(i<0x101 || i>=0x1B0){  // not slice code -> end of frame
#if 0
              // send END OF FRAME code:
              videobuffer[videobuf_len+0]=0;
              videobuffer[videobuf_len+1]=0;
              videobuffer[videobuf_len+2]=1;
              videobuffer[videobuf_len+3]=0xFF;
              videobuf_len+=4;
#endif
              if(!i) return -1; // EOF
              break;
            }
          } else {
	    if(i==0x100){
		pts=d_video->pts;
		d_video->pts=0;
	    }
            //if(i==0x100) in_frame=1; // picture startcode
            if(i>=0x101 && i<0x1B0) in_frame=1; // picture startcode
            else if(!i) return -1; // EOF
          }
	  //if(grab_frames==2 && (i==0x1B3 || i==0x1B8)) grab_frames=1;
          if(!read_video_packet(d_video)) return -1; // EOF
          //printf("read packet 0x%X, len=%d\n",i,videobuf_len);
	  // process headers:
	  switch(i){
	      case 0x1B3: mp_header_process_sequence_header (&picture, &videobuffer[start]);break;
	      case 0x1B5: mp_header_process_extension (&picture, &videobuffer[start]);break;
	      case 0x1B2: process_userdata (&videobuffer[start], videobuf_len-start);break;
	      case 0x100: picture_coding_type=(videobuffer[start+1] >> 3) & 7;break;
	  }
        }
        
        // if(videobuf_len>max_framesize) max_framesize=videobuf_len; // debug
        //printf("--- SEND %d bytes\n",videobuf_len);
//	if(grab_frames==1){
//	      FILE *f=fopen("grab.mpg","ab");
//	      fwrite(videobuffer,videobuf_len-4,1,f);
//	      fclose(f);
//	}

	*start=videobuffer; in_size=videobuf_len;
	//blit_frame=decode_video(video_out,sh_video,videobuffer,videobuf_len,drop_frame);

#if 1
    // get mpeg fps:
    //newfps=frameratecode2framerate[picture->frame_rate_code]*0.0001f;
    if(sh_video->fps!=picture.fps) if(!force_fps && !telecine){
            mp_msg(MSGT_CPLAYER,MSGL_WARN,"Warning! FPS changed %5.3f -> %5.3f  (%f) [%d]  \n",sh_video->fps,picture.fps,sh_video->fps-picture.fps,picture.frame_rate_code);
            sh_video->fps=picture.fps;
            sh_video->frametime=1.0/picture.fps;
    }
#endif

    // fix mpeg2 frametime:
    frame_time=(picture.display_time)*0.01f;
    picture.display_time=100;
    videobuf_len=0;

    telecine_cnt*=0.9; // drift out error
    telecine_cnt+=frame_time-5.0/4.0;
    mp_msg(MSGT_DECVIDEO,MSGL_DBG2,"\r telecine = %3.1f  %5.3f     \n",frame_time,telecine_cnt);
    
    if(telecine){
	frame_time=1;
	if(telecine_cnt<-1.5 || telecine_cnt>1.5){
	    mp_msg(MSGT_DECVIDEO,MSGL_INFO,MSGTR_LeaveTelecineMode);
	    telecine=0;
	}
    } else
	if(telecine_cnt>-0.5 && telecine_cnt<0.5 && !force_fps){
	    sh_video->fps=sh_video->fps*4/5;
	    sh_video->frametime=sh_video->frametime*5/4;
	    mp_msg(MSGT_DECVIDEO,MSGL_INFO,MSGTR_EnterTelecineMode);
	    telecine=1;
	}

  } else if((demuxer->file_format==DEMUXER_TYPE_MPEG4_ES) || ((demuxer->file_format==DEMUXER_TYPE_MPEG_TS) && (sh_video->format==0x10000004)) ||
            ((demuxer->file_format==DEMUXER_TYPE_MPEG_PS) && (sh_video->format==0x10000004))
  ){
      //
        while(videobuf_len<VIDEOBUFFER_SIZE-MAX_VIDEO_PACKET_SIZE){
          int i=sync_video_packet(d_video);
          if(!i) return -1;
          if(!read_video_packet(d_video)) return -1; // EOF
	  if(i==0x1B6) break;
        }
	*start=videobuffer; in_size=videobuf_len;
	videobuf_len=0;

  } else if(demuxer->file_format==DEMUXER_TYPE_H264_ES || ((demuxer->file_format==DEMUXER_TYPE_MPEG_TS) && (sh_video->format==0x10000005)) ||
            ((demuxer->file_format==DEMUXER_TYPE_MPEG_PS) && (sh_video->format==0x10000005))
  ){
      //
        int in_picture = 0;
        while(videobuf_len<VIDEOBUFFER_SIZE-MAX_VIDEO_PACKET_SIZE){
          int i=sync_video_packet(d_video);
          int pos = videobuf_len+4;
          if(!i) return -1;
          if(!read_video_packet(d_video)) return -1; // EOF
          if((i&~0x60) == 0x107 && i != 0x107) {
            h264_parse_sps(&picture, &(videobuffer[pos]), videobuf_len - pos);
            if(picture.fps > 0) {
              sh_video->fps=picture.fps;
              sh_video->frametime=1.0/picture.fps;
            }
            i=sync_video_packet(d_video);
            if(!i) return -1;
            if(!read_video_packet(d_video)) return -1; // EOF
          }

          // here starts the access unit end detection code
          // see the mail on MPlayer-dev-eng for details:
          // Date: Sat, 17 Sep 2005 11:24:06 +0200
          // Subject: Re: [MPlayer-dev-eng] [RFC] h264 ES parser problems
          // Message-ID: <20050917092406.GA7699@rz.uni-karlsruhe.de>
          if((i&~0x60) == 0x101 || (i&~0x60) == 0x102 || (i&~0x60) == 0x105)
            // found VCL NAL with slice header i.e. start of current primary coded
            // picture, so start scanning for the end now
            in_picture = 1;
          if (in_picture) {
            i = sync_video_packet(d_video) & ~0x60; // code of next packet
            if(i == 0x106 || i == 0x109) break; // SEI or access unit delim.
            if(i == 0x101 || i == 0x102 || i == 0x105) {
              // assuming arbitrary slice ordering is not allowed, the
              // first_mb_in_slice (golomb encoded) value should be 0 then
              // for the first VCL NAL in a picture
              if (demux_peekc(d_video) & 0x80)
                break;
            }
          }
        }
	*start=videobuffer; in_size=videobuf_len;
	videobuf_len=0;

  } else {
      // frame-based file formats: (AVI,ASF,MOV)
    in_size=ds_get_packet(d_video,start);
    if(in_size<0) return -1; // EOF
//    if(in_size>max_framesize) max_framesize=in_size;
//    blit_frame=decode_video(video_out,sh_video,start,in_size,drop_frame);
  }

//  vdecode_time=video_time_usage-vdecode_time;

//------------------------ frame decoded. --------------------

    // Increase video timers:
    sh_video->num_frames+=frame_time;
    ++sh_video->num_frames_decoded;

    frame_time*=sh_video->frametime;

    // override frame_time for variable/unknown FPS formats:
    if(!force_fps) switch(demuxer->file_format){
      case DEMUXER_TYPE_GIF:
      case DEMUXER_TYPE_MATROSKA:
	if(d_video->pts>0 && pts1>0 && d_video->pts>pts1)
	  frame_time=d_video->pts-pts1;
        break;
#ifdef USE_TV
      case DEMUXER_TYPE_TV:
#endif
      case DEMUXER_TYPE_MOV:
      case DEMUXER_TYPE_FILM:
      case DEMUXER_TYPE_VIVO:
      case DEMUXER_TYPE_OGG:
      case DEMUXER_TYPE_ASF: {
        float next_pts = ds_get_next_pts(d_video);
        float d= next_pts > 0 ? next_pts - d_video->pts : d_video->pts-pts1;
        if(d>=0){
          if(d>0){
            /* 10000 is used for OGM only */
            if((int)sh_video->fps==1000||(int)sh_video->fps==10000)
              mp_msg(MSGT_CPLAYER,MSGL_V,"\navg. framerate: %d fps             \n",(int)(1.0f/d));
	    sh_video->frametime=d; // 1ms
            sh_video->fps=1.0f/d;
	  }
          frame_time = d;
        } else {
          mp_msg(MSGT_CPLAYER,MSGL_WARN,"\nInvalid frame duration value (%5.3f/%5.3f => %5.3f). Defaulting to %5.3f sec.\n",d_video->pts,next_pts,d,frame_time);
          // frame_time = 1/25.0;
        }
      }
      break;
      case DEMUXER_TYPE_REAL:
      case DEMUXER_TYPE_LAVF:
        if((int)sh_video->fps==1000 || (int)sh_video->fps<=1){
          float next_pts = ds_get_next_pts(d_video);
          float d= next_pts > 0 ? next_pts - d_video->pts : d_video->pts-pts1;
          if(d>=0){
            frame_time = d;
          } 
        }
      break;
    }
    
    if(demuxer->file_format==DEMUXER_TYPE_MPEG_PS ||
       demuxer->file_format==DEMUXER_TYPE_MPEG_PES ||
       ((demuxer->file_format==DEMUXER_TYPE_MPEG_TS) && ((sh_video->format==0x10000001) || (sh_video->format==0x10000002))) ||
       demuxer->file_format==DEMUXER_TYPE_MPEG_ES ||
       demuxer->file_format==DEMUXER_TYPE_MPEG_TY){

//	if(pts>0.0001) printf("\r!!! pts: %5.3f [%d] (%5.3f)   \n",pts,picture_coding_type,i_pts);

	sh_video->pts+=frame_time;
	if(picture_coding_type<=2 && sh_video->i_pts){
//	    printf("XXX predict: %5.3f pts: %5.3f error=%5.5f   \n",i_pts,d_video->pts2,i_pts-d_video->pts2);
	    sh_video->pts=sh_video->i_pts;
	    sh_video->i_pts=pts;
	} else {
	    if(pts){
		if(picture_coding_type<=2) sh_video->i_pts=pts;
		else {
//		    printf("BBB predict: %5.3f pts: %5.3f error=%5.5f   \n",pts,d_video->pts2,pts-d_video->pts2);
		    sh_video->pts=pts;
		}
	    }
	}
//	printf("\rIII pts: %5.3f [%d] (%5.3f)   \n",d_video->pts2,picture_coding_type,i_pts);
    } else
	sh_video->pts=d_video->pts;
    
    if(frame_time_ptr) *frame_time_ptr=frame_time;
    return in_size;

}

