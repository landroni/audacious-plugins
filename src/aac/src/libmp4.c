#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include "faad.h"
#include "mp4ff.h"
#include "tagging.h"

#include <audacious/plugin.h>
#include <audacious/output.h>
#include <audacious/util.h>
#include <audacious/titlestring.h>
#include <audacious/vfs.h>

#define MP4_VERSION	VERSION

/*
 * BUFFER_SIZE is the highest amount of memory that can be pulled.
 * We use this for sanity checks, among other things, as mp4ff needs
 * a labotomy sometimes.
 */
#define BUFFER_SIZE	FAAD_MIN_STREAMSIZE*64

/*
 * AAC_MAGIC is the pattern that marks the beginning of an MP4 container.
 */
#define AAC_MAGIC     (unsigned char [4]) { 0xFF, 0xF9, 0x5C, 0x80 }

static void	mp4_init(void);
static void	mp4_about(void);
static void	mp4_play(char *);
static void	mp4_stop(void);
static void	mp4_pause(short);
static void	mp4_seek(int);
static int	mp4_getTime(void);
static void	mp4_cleanup(void);
static int	mp4_IsOurFD(char *, VFSFile *);
static int	mp4_IsOurFile(char *);
static void	mp4_getSongTitle(char *filename, char **, int *);
static void*	mp4Decode(void *);
static TitleInput *mp4_get_song_tuple(char *);

void     audmp4_file_info_box(gchar *);
gboolean buffer_playing;

InputPlugin mp4_ip =
  {
    0,	// handle
    0,	// filename
    "MP4 Audio Plugin",
    mp4_init,
    mp4_about,
    0,	// configuration
    mp4_IsOurFile,
    0,	//scandir
    mp4_play,
    mp4_stop,
    mp4_pause,
    mp4_seek,
    0,	// set equalizer
    mp4_getTime,
    0,	// get volume
    0,
    mp4_cleanup,
    0,	// obsolete
    0,	// send visualisation data
    0,	// set player window info
    0,	// set song title text
    mp4_getSongTitle,	// get song title text
    0,  // info box
    0,	// to output plugin
    mp4_get_song_tuple,
    0,
    0,
    mp4_IsOurFD,
  };

typedef struct  _mp4cfg{
  gshort        file_type;
#define FILE_UNKNOW     0
#define FILE_MP4        1
#define FILE_AAC        2
}               Mp4Config;

static Mp4Config	mp4cfg;
static GThread		*decodeThread;
GStaticMutex 		mutex = G_STATIC_MUTEX_INIT;
static int		seekPosition = -1;

void getMP4info(char*);
int getAACTrack(mp4ff_t *);

static uint32_t mp4_read_callback(void *data, void *buffer, uint32_t len)
{
	if (data == NULL || buffer == NULL)
		return -1;

	return vfs_fread(buffer, 1, len, (VFSFile *) data);
}

static uint32_t mp4_seek_callback(void *data, uint64_t pos)
{
	if (data == NULL)
		return -1;

	return vfs_fseek((VFSFile *) data, pos, SEEK_SET);
}

/*
 * Function extname (filename)
 *
 *    Return pointer within filename to its extenstion, or NULL if
 *    filename has no extension.
 *
 */
static gchar *
extname(const char *filename)
{   
    gchar *ext = strrchr(filename, '.');

    if (ext != NULL)
        ++ext;

    return ext;
}

InputPlugin *get_iplugin_info(void)
{
  return(&mp4_ip);
}

static void mp4_init(void)
{
  mp4cfg.file_type = FILE_UNKNOW;
  seekPosition = -1;
  return;
}

static void mp4_play(char *filename)
{
  buffer_playing = TRUE;
  decodeThread = g_thread_create((GThreadFunc)mp4Decode, g_strdup(filename), TRUE, NULL);
  return;
}

static void mp4_stop(void)
{
  if(buffer_playing){
    buffer_playing = FALSE;
    mp4_ip.output->close_audio();
    g_thread_join(decodeThread);
  }
}

static int	mp4_IsOurFile(char *filename)
{
  VFSFile *file;
  gchar* extension;
  gchar magic[8];
 
  extension = strrchr(filename, '.');
  if ((file = vfs_fopen(filename, "rb"))) {
      vfs_fread(magic, 1, 8, file);
      if (!memcmp(magic, AAC_MAGIC, 4)) {
           vfs_fclose(file);
           return 1;
      }
      if (!memcmp(magic, "ID3", 3)) {		// ID3 tag bolted to the front, obfuscated magic bytes
           vfs_fclose(file);
           if (extension &&(
	      !strcasecmp(extension, ".mp4") ||	// official extension
	      !strcasecmp(extension, ".m4a") ||	// Apple mp4 extension
	      !strcasecmp(extension, ".aac")	// old MPEG2/4-AAC extension
	   ))
	      return 1;
	   else
	      return 0;
      }
      if (!memcmp(&magic[4], "ftyp", 4)) {
           vfs_fclose(file);
           return 1;
      }
      vfs_fclose(file);
  }
  return 0;
}

static int	mp4_IsOurFD(char *filename, VFSFile* file)
{
  gchar* extension;
  gchar magic[8];

  extension = strrchr(filename, '.');
  vfs_fread(magic, 1, 8, file);
  if (!memcmp(magic, AAC_MAGIC, 4))
    return 1;
  if (!memcmp(&magic[4], "ftyp", 4))
    return 1;
  if (!memcmp(magic, "ID3", 3)) {		// ID3 tag bolted to the front, obfuscated magic bytes
    if (extension &&(
      !strcasecmp(extension, ".mp4") ||	// official extension
      !strcasecmp(extension, ".m4a") ||	// Apple mp4 extension
      !strcasecmp(extension, ".aac")	// old MPEG2/4-AAC extension
    ))
      return 1;
    else
      return 0;
  }
  return 0;
}

static void	mp4_about(void)
{
  static GtkWidget *aboutbox;

  if(aboutbox!=NULL)
    return;
  aboutbox = xmms_show_message("About MP4 AAC player plugin",
			       "Using libfaad2-" FAAD2_VERSION " for decoding.\n"
			       "Copyright (c) 2005-2006 Audacious team",
			       "Ok", FALSE, NULL, NULL);
  g_signal_connect(G_OBJECT(aboutbox), "destroy",
                     G_CALLBACK(gtk_widget_destroyed),
                     &aboutbox);
}

static void	mp4_pause(short flag)
{
  mp4_ip.output->pause(flag);
}

static void	mp4_seek(int time)
{
  seekPosition = time;
  while(buffer_playing && seekPosition!=-1)
    xmms_usleep(10000);
}

static int	mp4_getTime(void)
{
  if(!buffer_playing)
    return (-1);
  else
    return (mp4_ip.output->output_time());
}

static void	mp4_cleanup(void)
{
}

static TitleInput   *mp4_get_song_tuple(char *fn)
{
	mp4ff_callback_t *mp4cb = g_malloc0(sizeof(mp4ff_callback_t));
	VFSFile *mp4fh;
	mp4ff_t *mp4file;
	TitleInput *input = NULL;
	gchar *filename = g_strdup(fn);

	mp4fh = vfs_fopen(filename, "rb");
	mp4cb->read = mp4_read_callback;
	mp4cb->seek = mp4_seek_callback;
	mp4cb->user_data = mp4fh;	

	if (!(mp4file = mp4ff_open_read(mp4cb))) {
		g_free(mp4cb);
		vfs_fclose(mp4fh);
	} else {
		gint mp4track= getAACTrack(mp4file);
		gint numSamples = mp4ff_num_samples(mp4file, mp4track);
		guint framesize = 1024;
		gulong samplerate;
		guchar channels;
		gint msDuration;
		mp4AudioSpecificConfig mp4ASC;
		gchar *tmpval;
		guchar *buffer = NULL;
		guint bufferSize = 0;
		faacDecHandle decoder;

		if (mp4track == -1)
			return NULL;

		decoder = faacDecOpen();
		mp4ff_get_decoder_config(mp4file, mp4track, &buffer, &bufferSize);

		if ( !buffer ) {
			faacDecClose(decoder);
			return FALSE;
		}
		if ( faacDecInit2(decoder, buffer, bufferSize, 
				  &samplerate, &channels) < 0 ) {
			faacDecClose(decoder);

			return FALSE;
		}

		/* Add some hacks for SBR profile */
		if (AudioSpecificConfig(buffer, bufferSize, &mp4ASC) >= 0) {
			if (mp4ASC.frameLengthFlag == 1) framesize = 960;
			if (mp4ASC.sbr_present_flag == 1) framesize *= 2;
		}
			
		g_free(buffer);

		faacDecClose(decoder);

		msDuration = ((float)numSamples * (float)(framesize - 1.0)/(float)samplerate) * 1000;

		input = bmp_title_input_new();

		mp4ff_meta_get_title(mp4file, &input->track_name);
		mp4ff_meta_get_album(mp4file, &input->album_name);
		mp4ff_meta_get_artist(mp4file, &input->performer);
		mp4ff_meta_get_date(mp4file, &tmpval);
		mp4ff_meta_get_genre(mp4file, &input->genre);

		if (tmpval)
		{
			input->year = atoi(tmpval);
			free(tmpval);
		}

		input->file_name = g_path_get_basename(filename);
		input->file_path = g_path_get_dirname(filename);
		input->file_ext = extname(filename);
		input->length = msDuration;

		free (mp4cb);
		vfs_fclose(mp4fh);
	}

	return input;
}

static gchar   *mp4_get_song_title(char *filename)
{
	mp4ff_callback_t *mp4cb = g_malloc0(sizeof(mp4ff_callback_t));
	VFSFile *mp4fh;
	mp4ff_t *mp4file;
	gchar *title = NULL;

	mp4fh = vfs_fopen(filename, "rb");
	mp4cb->read = mp4_read_callback;
	mp4cb->seek = mp4_seek_callback;
	mp4cb->user_data = mp4fh;	

	if (!(mp4file = mp4ff_open_read(mp4cb))) {
		g_free(mp4cb);
		vfs_fclose(mp4fh);
	} else {
		TitleInput *input;
		gchar *tmpval;

		input = bmp_title_input_new();

		mp4ff_meta_get_title(mp4file, &input->track_name);
		mp4ff_meta_get_album(mp4file, &input->album_name);
		mp4ff_meta_get_artist(mp4file, &input->performer);
		mp4ff_meta_get_date(mp4file, &tmpval);
		mp4ff_meta_get_genre(mp4file, &input->genre);

		if (tmpval)
		{
			input->year = atoi(tmpval);
			free(tmpval);
		}

		input->file_name = g_path_get_basename(filename);
		input->file_path = g_path_get_dirname(filename);
		input->file_ext = extname(filename);

		title = xmms_get_titlestring(xmms_get_gentitle_format(), input);

		free (input->track_name);
		free (input->album_name);
		free (input->performer);
		free (input->genre);
		free (input->file_name);
		free (input->file_path);
		free (input);

		free (mp4cb);
		vfs_fclose(mp4fh);
	}

	if (!title)
	{
		title = g_path_get_basename(filename);
		if (extname(title))
			*(extname(title) - 1) = '\0';
	}

	return title;
}

static void	mp4_getSongTitle(char *filename, char **title_real, int *len_real)
{
	(*title_real) = mp4_get_song_title(filename);
	(*len_real) = -1;
}

static int my_decode_mp4( char *filename, mp4ff_t *mp4file )
{
	// We are reading a MP4 file
	gint mp4track= getAACTrack(mp4file);

	if ( mp4track < 0 ) {
		//TODO: check here for others Audio format.....
		g_print("Unsupported Audio track type\n");
	}
	else {
		faacDecHandle	decoder;
		unsigned char	*buffer	= NULL;
		guint		bufferSize = 0;
		gulong		samplerate;
		guchar		channels;
		//guint		avgBitrate;
		gulong		msDuration;
		gulong		numSamples;
		gulong		sampleID = 1;
		unsigned int	framesize = 1024;
		mp4AudioSpecificConfig mp4ASC;

		gchar	     *xmmstitle = NULL;

		xmmstitle = mp4_get_song_title(filename);

		if(xmmstitle == NULL)
			xmmstitle = g_strdup(filename);

		decoder = faacDecOpen();
		mp4ff_get_decoder_config(mp4file, mp4track, &buffer, &bufferSize);
		if ( !buffer ) {
			faacDecClose(decoder);
			return FALSE;
		}
		if ( faacDecInit2(decoder, buffer, bufferSize, 
				  &samplerate, &channels) < 0 ) {
			faacDecClose(decoder);

			return FALSE;
		}

		/* Add some hacks for SBR profile */
		if (AudioSpecificConfig(buffer, bufferSize, &mp4ASC) >= 0) {
			if (mp4ASC.frameLengthFlag == 1) framesize = 960;
			if (mp4ASC.sbr_present_flag == 1) framesize *= 2;
		}
			
		g_free(buffer);
		if( !channels ) {
			faacDecClose(decoder);

			return FALSE;
		}
		numSamples = mp4ff_num_samples(mp4file, mp4track);
		msDuration = ((float)numSamples * (float)(framesize - 1.0)/(float)samplerate) * 1000;
		mp4_ip.output->open_audio(FMT_S16_NE, samplerate, channels);
		mp4_ip.output->flush(0);

		mp4_ip.set_info(xmmstitle, msDuration, 
				mp4ff_get_avg_bitrate( mp4file, mp4track ), 
				samplerate,channels);

		while ( buffer_playing ) {
			void*			sampleBuffer;
			faacDecFrameInfo	frameInfo;    
			gint			rc;

			/* Seek if seek position has changed */
			if ( seekPosition!=-1 ) {
				sampleID =  (float)seekPosition*(float)samplerate/(float)(framesize - 1.0);
				mp4_ip.output->flush(seekPosition*1000);
				seekPosition = -1;
			}

			/* Otherwise continue playing */
			buffer=NULL;
			bufferSize=0;

			/* If we've run to the end of the file, we're done. */
			if(sampleID >= numSamples){
				/* Finish playing before we close the
				   output. */
				while ( mp4_ip.output->buffer_playing() ) {
					xmms_usleep(10000);
				}

				mp4_ip.output->flush(seekPosition*1000);
				mp4_ip.output->close_audio();
				faacDecClose(decoder);

				g_static_mutex_lock(&mutex);
				buffer_playing = FALSE;
				g_static_mutex_unlock(&mutex);
				g_thread_exit(NULL);

				return FALSE;
			}
			rc= mp4ff_read_sample(mp4file, mp4track, 
					  sampleID++, &buffer, &bufferSize);

			/*g_print(":: %d/%d\n", sampleID-1, numSamples);*/

			/* If we can't read the file, we're done. */
			if((rc == 0) || (buffer== NULL) || (bufferSize == 0) || (bufferSize > BUFFER_SIZE)){
				g_print("MP4: read error\n");
				sampleBuffer = NULL;
				sampleID=0;
				mp4_ip.output->buffer_free();
				mp4_ip.output->close_audio();

				faacDecClose(decoder);

				return FALSE;
			}

/*			g_print(" :: %d/%d\n", bufferSize, BUFFER_SIZE); */

			sampleBuffer= faacDecDecode(decoder, 
						    &frameInfo, 
						    buffer, 
						    bufferSize);

			/* If there was an error decoding, we're done. */
			if(frameInfo.error > 0){
				g_print("MP4: %s\n",
					faacDecGetErrorMessage(frameInfo.error));
				mp4_ip.output->close_audio();
				faacDecClose(decoder);

				return FALSE;
			}
			if(buffer){
				g_free(buffer);
				buffer=NULL;
				bufferSize=0;
			}
			produce_audio(mp4_ip.output->written_time(),
					   FMT_S16_NE,
					   channels,
					   frameInfo.samples<<1,
					   sampleBuffer, &buffer_playing);
		}
		mp4_ip.output->close_audio();

		faacDecClose(decoder);
	}

	return TRUE;
}

static void my_decode_aac( char *filename )
{
	// WE ARE READING AN AAC FILE
	VFSFile		*file = NULL;
	faacDecHandle	decoder = 0;
	guchar		*buffer = 0;
	gulong		bufferconsumed = 0;
	gulong		samplerate = 0;
	guchar		channels;
	gulong		buffervalid = 0;
	TitleInput*	input;
	gchar		*temp = g_strdup(filename);
	gchar		*ext  = strrchr(temp, '.');
	gchar		*xmmstitle = NULL;
	faacDecConfigurationPtr config;

	if((file = vfs_fopen(filename, "rb")) == 0){
		g_print("AAC: can't find file %s\n", filename);
		buffer_playing = FALSE;
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}
	if((decoder = faacDecOpen()) == NULL){
		g_print("AAC: Open Decoder Error\n");
		vfs_fclose(file);
		buffer_playing = FALSE;
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}
	config = faacDecGetCurrentConfiguration(decoder);
	config->useOldADTSFormat = 0;
	faacDecSetConfiguration(decoder, config);
	if((buffer = g_malloc(BUFFER_SIZE)) == NULL){
		g_print("AAC: error g_malloc\n");
		vfs_fclose(file);
		buffer_playing = FALSE;
		faacDecClose(decoder);
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}
	if((buffervalid = vfs_fread(buffer, 1, BUFFER_SIZE, file))==0){
		g_print("AAC: Error reading file\n");
		g_free(buffer);
		vfs_fclose(file);
		buffer_playing = FALSE;
		faacDecClose(decoder);
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}
	XMMS_NEW_TITLEINPUT(input);
	input->file_name = (char*)g_basename(temp);
	input->file_ext = ext ? ext+1 : NULL;
	input->file_path = temp;
	if(!strncmp((char*)buffer, "ID3", 3)){
		gint size = 0;

		vfs_fseek(file, 0, SEEK_SET);
		size = (buffer[6]<<21) | (buffer[7]<<14) | (buffer[8]<<7) | buffer[9];
		size+=10;
		vfs_fread(buffer, 1, size, file);
		buffervalid = vfs_fread(buffer, 1, BUFFER_SIZE, file);
	}
	xmmstitle = xmms_get_titlestring(xmms_get_gentitle_format(), input);
	if(xmmstitle == NULL)
		xmmstitle = g_strdup(input->file_name);
	if(temp) g_free(temp);
	if(input->performer) g_free(input->performer);
	if(input->album_name) g_free(input->album_name);
	if(input->track_name) g_free(input->track_name);
	if(input->genre) g_free(input->genre);
	g_free(input);
	bufferconsumed = faacDecInit(decoder,
				     buffer,
				     buffervalid,
				     &samplerate,
				     &channels);
	if(mp4_ip.output->open_audio(FMT_S16_NE,samplerate,channels) == FALSE){
		g_print("AAC: Output Error\n");
		g_free(buffer); buffer=0;
		faacDecClose(decoder);
		vfs_fclose(file);
		mp4_ip.output->close_audio();
		g_free(xmmstitle);
		buffer_playing = FALSE;
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}

	mp4_ip.set_info(xmmstitle, -1, -1, samplerate, channels);
	mp4_ip.output->flush(0);

	while(buffer_playing && buffervalid > 0){
		faacDecFrameInfo	finfo;
		unsigned long	samplesdecoded;
		char*		sample_buffer = NULL;

		if(bufferconsumed > 0){
			memmove(buffer, &buffer[bufferconsumed], buffervalid-bufferconsumed);
			buffervalid -= bufferconsumed;
			buffervalid += vfs_fread(&buffer[buffervalid], 1,
					     BUFFER_SIZE-buffervalid, file);
			bufferconsumed = 0;
		}
		sample_buffer = faacDecDecode(decoder, &finfo, buffer, buffervalid);
		if(finfo.error){
			config = faacDecGetCurrentConfiguration(decoder);
			if(config->useOldADTSFormat != 1){
				faacDecClose(decoder);
				decoder = faacDecOpen();
				config = faacDecGetCurrentConfiguration(decoder);
				config->useOldADTSFormat = 1;
				faacDecSetConfiguration(decoder, config);
				finfo.bytesconsumed=0;
				finfo.samples = 0;
				faacDecInit(decoder,
					    buffer,
					    buffervalid,
					    &samplerate,
					    &channels);
			}else{
				g_print("FAAD2 Warning %s\n", faacDecGetErrorMessage(finfo.error));
				buffervalid = 0;
			}
		}
		bufferconsumed += finfo.bytesconsumed;
		samplesdecoded = finfo.samples;
		if((samplesdecoded<=0) && !sample_buffer){
			g_print("AAC: error sample decoding\n");
			continue;
		}
		produce_audio(mp4_ip.output->written_time(),
				   FMT_S16_LE, channels,
				   samplesdecoded<<1, sample_buffer, &buffer_playing);
	}
	mp4_ip.output->buffer_free();
	mp4_ip.output->close_audio();
	buffer_playing = FALSE;
	g_free(buffer);
	faacDecClose(decoder);
	g_free(xmmstitle);
	vfs_fclose(file);
	seekPosition = -1;

	buffer_playing = FALSE;
	g_static_mutex_unlock(&mutex);
	g_thread_exit(NULL);
}

static void *mp4Decode( void *args )
{
	mp4ff_callback_t *mp4cb = g_malloc0(sizeof(mp4ff_callback_t));
	VFSFile *mp4fh;
	mp4ff_t *mp4file;

	char* url= (char*)args;
	char filename[255];
	memset( filename, '\0', 255 );

#if 0
	/* If we have a URL-style string, de-URLise it... */
	if ( !strncmp( url, "file://", 7 ) ) {
		char *output= curl_unescape( url, 0 );
		char *tmp= output+7;

		strncpy( filename, tmp, 254 );

		curl_free( output );
	}
	else {
		strncpy( filename, url, 254 );
	}
#endif

	strncpy( filename, url, 254 );

	mp4fh = vfs_fopen(filename, "rb");
	mp4cb->read = mp4_read_callback;
	mp4cb->seek = mp4_seek_callback;
	mp4cb->user_data = mp4fh;

	g_static_mutex_lock(&mutex);
	seekPosition= -1;
	buffer_playing= TRUE;
	g_static_mutex_unlock(&mutex);

	mp4file= mp4ff_open_read(mp4cb);
	if( !mp4file ) {
		mp4cfg.file_type = FILE_AAC;
		vfs_fclose(mp4fh);
		g_free(mp4cb);
	}
	else {
		mp4cfg.file_type = FILE_MP4;
	}

	if ( mp4cfg.file_type == FILE_MP4 ) {
		my_decode_mp4( filename, mp4file );

		g_free(args);
		vfs_fclose(mp4fh);
		g_static_mutex_lock(&mutex);
		buffer_playing = FALSE;
		g_static_mutex_unlock(&mutex);
		g_thread_exit(NULL);
	}
	else {
		my_decode_aac( filename );
	}

	return NULL;
}
