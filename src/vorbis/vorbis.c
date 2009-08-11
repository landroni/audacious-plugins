/*
 * Copyright (C) Tony Arcieri <bascule@inferno.tusculum.edu>
 * Copyright (C) 2001-2002  Haavard Kvaalen <havardk@xmms.org>
 * Copyright (C) 2007 William Pitcock <nenolod@sacredspiral.co.uk>
 * Copyright (C) 2008 Cristi Măgherușan <majeru@gentoo.ro>
 * Copyright (C) 2008 Eugene Zagidullin <e.asphyx@gmail.com>
 *
 * ReplayGain processing Copyright (C) 2002 Gian-Carlo Pascutto <gcp@sjeng.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include "config.h"
/*#define AUD_DEBUG
#define DEBUG*/

#define REMOVE_NONEXISTANT_TAG(x)   if (x != NULL && !*x) { x = NULL; }

#include <glib.h>

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include <audacious/plugin.h>
#include <audacious/i18n.h>

#include "vorbis.h"

extern vorbis_config_t vorbis_cfg;

static long vorbis_interleave_buffer(float **pcm, int samples, int ch,
                                     float *pcmout);

static size_t ovcb_read(void *ptr, size_t size, size_t nmemb,
                        void *datasource);
static int ovcb_seek(void *datasource, int64_t offset, int whence);
static int ovcb_close(void *datasource);
static long ovcb_tell(void *datasource);

ov_callbacks vorbis_callbacks = {
    ovcb_read,
    ovcb_seek,
    ovcb_close,
    ovcb_tell
};

ov_callbacks vorbis_callbacks_stream = {
    ovcb_read,
    NULL,
    ovcb_close,
    NULL
};

static volatile gint seek_value = -1;
static GMutex * seek_mutex = NULL;
static GCond * seek_cond = NULL;

gchar **vorbis_tag_encoding_list = NULL;


static gint
vorbis_check_fd(const gchar *filename, VFSFile *stream)
{
    OggVorbis_File vfile;
    gint result;
    VFSVorbisFile *fd;

    fd = g_new0(VFSVorbisFile, 1);
    fd->fd = stream;
    fd->probe = TRUE;

    /*
     * The open function performs full stream detection and machine
     * initialization.  If it returns zero, the stream *is* Vorbis and
     * we're fully ready to decode.
     */

    memset(&vfile, 0, sizeof(vfile));

    result = ov_test_callbacks(fd, &vfile, NULL, 0, aud_vfs_is_streaming(stream) ? vorbis_callbacks_stream : vorbis_callbacks);

    switch (result) {
    case OV_EREAD:
#ifdef DEBUG
        g_message("** vorbis.c: Media read error: %s", filename);
#endif
        return FALSE;
        break;
    case OV_ENOTVORBIS:
#ifdef DEBUG
        g_message("** vorbis.c: Not Vorbis data: %s", filename);
#endif
        return FALSE;
        break;
    case OV_EVERSION:
#ifdef DEBUG
        g_message("** vorbis.c: Version mismatch: %s", filename);
#endif
        return FALSE;
        break;
    case OV_EBADHEADER:
#ifdef DEBUG
        g_message("** vorbis.c: Invalid Vorbis bistream header: %s",
                  filename);
#endif
        return FALSE;
        break;
    case OV_EFAULT:
#ifdef DEBUG
        g_message("** vorbis.c: Internal logic fault while reading %s",
                  filename);
#endif
        return FALSE;
        break;
    case 0:
        break;
    default:
        break;
    }

    ov_clear(&vfile);           /* once the ov_open succeeds, the stream belongs to
                                   vorbisfile.a.  ov_clear will fclose it */
    return TRUE;
}

static void
set_tuple_str(Tuple *tuple, const gint nfield, const gchar *field,
    vorbis_comment *comment, gchar *key)
{
    gchar *str = vorbis_comment_query(comment, key, 0);
    if (str != NULL) {
        gchar *tmp = aud_str_to_utf8(str);
        aud_tuple_associate_string(tuple, nfield, field, tmp);
        g_free(tmp);
    }
}

static Tuple *
get_aud_tuple_for_vorbisfile(OggVorbis_File * vorbisfile, const gchar *filename)
{
    VFSVorbisFile *vfd = (VFSVorbisFile *) vorbisfile->datasource;
    Tuple *tuple;
    gint length;
    vorbis_comment *comment = NULL;

    tuple = aud_tuple_new_from_filename(filename);

    length = aud_vfs_is_streaming(vfd->fd) ? -1 : ov_time_total(vorbisfile, -1) * 1000;

    /* associate with tuple */
    aud_tuple_associate_int(tuple, FIELD_LENGTH, NULL, length);
    /* maybe, it would be better to display nominal bitrate (like in main win), not average? --eugene */
    aud_tuple_associate_int(tuple, FIELD_BITRATE, NULL, ov_bitrate(vorbisfile, -1) / 1000);

    if ((comment = ov_comment(vorbisfile, -1)) != NULL) {
        gchar *tmps;
        set_tuple_str(tuple, FIELD_TITLE, NULL, comment, "title");
        set_tuple_str(tuple, FIELD_ARTIST, NULL, comment, "artist");
        set_tuple_str(tuple, FIELD_ALBUM, NULL, comment, "album");
        set_tuple_str(tuple, FIELD_DATE, NULL, comment, "date");
        set_tuple_str(tuple, FIELD_GENRE, NULL, comment, "genre");
        set_tuple_str(tuple, FIELD_COMMENT, NULL, comment, "comment");

        if ((tmps = vorbis_comment_query(comment, "tracknumber", 0)) != NULL)
            aud_tuple_associate_int(tuple, FIELD_TRACK_NUMBER, NULL, atoi(tmps));
    }

    aud_tuple_associate_string(tuple, FIELD_QUALITY, NULL, "lossy");

    if (comment != NULL && comment->vendor != NULL)
    {
        gchar *codec = g_strdup_printf("Ogg Vorbis [%s]", comment->vendor);
        aud_tuple_associate_string(tuple, FIELD_CODEC, NULL, codec);
        g_free(codec);
    }
    else
        aud_tuple_associate_string(tuple, FIELD_CODEC, NULL, "Ogg Vorbis");

    aud_tuple_associate_string(tuple, FIELD_MIMETYPE, NULL, "application/ogg");

    return tuple;
}

static gchar *
vorbis_generate_title(OggVorbis_File * vorbisfile, const gchar * filename)
{
    gchar *displaytitle, *tmp;
    Tuple *input;

    input = get_aud_tuple_for_vorbisfile(vorbisfile, filename);

    displaytitle = aud_tuple_formatter_make_title_string(
        input, vorbis_cfg.tag_override ? vorbis_cfg.tag_format : aud_get_gentitle_format());

    if ((tmp = aud_vfs_get_metadata(((VFSVorbisFile *) vorbisfile->datasource)->fd, "stream-name")) != NULL)
    {
        gchar *old = displaytitle;

        aud_tuple_associate_string(input, -1, "stream", tmp);
        aud_tuple_associate_string(input, FIELD_TITLE, NULL, old);

        displaytitle = aud_tuple_formatter_process_string(input, "${?title:${title}}${?stream: (${stream})}");

        g_free(old);
        g_free(tmp);
    }

    aud_tuple_free(input);

    return displaytitle;
}

static gboolean
vorbis_update_replaygain(OggVorbis_File *vf, ReplayGainInfo *rg_info)
{
    vorbis_comment *comment;
    gchar *rg_gain, *rg_peak;

    if (vf == NULL || rg_info == NULL || (comment = ov_comment(vf, -1)) == NULL)
        return FALSE;

    rg_gain = vorbis_comment_query(comment, "replaygain_album_gain", 0);
    if (!rg_gain) rg_gain = vorbis_comment_query(comment, "rg_audiophile", 0);    /* Old */
    rg_info->album_gain = rg_gain != NULL ? atof(rg_gain) : 0.0;

    rg_gain = vorbis_comment_query(comment, "replaygain_track_gain", 0);
    if (!rg_gain) rg_gain = vorbis_comment_query(comment, "rg_radio", 0);    /* Old */
    rg_info->track_gain = rg_gain != NULL ? atof(rg_gain) : 0.0;

    rg_peak = vorbis_comment_query(comment, "replaygain_album_peak", 0);
    rg_info->album_peak = rg_peak != NULL ? atof(rg_peak) : 0.0;

    rg_peak = vorbis_comment_query(comment, "replaygain_track_peak", 0);
    if (!rg_peak) rg_peak = vorbis_comment_query(comment, "rg_peak", 0);  /* Old */
    rg_info->track_peak = rg_peak != NULL ? atof(rg_peak) : 0.0;

    return TRUE;
}

static long
vorbis_interleave_buffer(float **pcm, int samples, int ch, float *pcmout)
{
    int i, j;
    for (i = 0; i < samples; i++)
        for (j = 0; j < ch; j++)
            *pcmout++ = pcm[j][i];

    return ch * samples * sizeof(float);
}


#define PCM_FRAMES 1024
#define PCM_BUFSIZE (PCM_FRAMES * 2)

static void
vorbis_play(InputPlayback *playback)
{
    gchar *title = NULL;
    gboolean streaming;
    vorbis_info *vi;
    OggVorbis_File vf;
    VFSVorbisFile *fd = NULL;
    VFSFile *stream = NULL;
    glong timercount = 0;
    gint last_section = -1;
    ReplayGainInfo rg_info;
    gfloat pcmout[PCM_BUFSIZE*sizeof(float)], **pcm;
    gint bytes, channels, samplerate, br, duration;

    playback->error = FALSE;
    seek_value = -1;
    memset(&vf, 0, sizeof(vf));

    if ((stream = aud_vfs_fopen(playback->filename, "r")) == NULL) {
        playback->eof = TRUE;
        goto play_cleanup;
    }

    fd = g_new0(VFSVorbisFile, 1);
    fd->fd = stream;

    streaming = aud_vfs_is_streaming(fd->fd);
    if (ov_open_callbacks(fd, &vf, NULL, 0, streaming ? vorbis_callbacks_stream : vorbis_callbacks) < 0) {
        vorbis_callbacks.close_func(fd);
        playback->eof = TRUE;
        goto play_cleanup;
    }

    vi = ov_info(&vf, -1);

    if (vi->channels > 2)
    {
        playback->eof = TRUE;
        goto play_cleanup;
    }

    duration = streaming ? -1 : ov_time_total(&vf, -1);
    br = vi->bitrate_nominal;
    channels = vi->channels;
    samplerate = vi->rate;

    title = vorbis_generate_title(&vf, playback->filename);
    vorbis_update_replaygain(&vf, &rg_info);
    playback->set_replaygain_info(playback, &rg_info);
    playback->set_params(playback, title, duration * 1000, br, samplerate, channels);
    if (!playback->output->open_audio(FMT_FLOAT, samplerate, channels)) {
        playback->error = TRUE;
        goto play_cleanup;
    }

    playback->playing = 1;
    playback->eof = 0;
    playback->set_pb_ready(playback);

    /*
     * Note that chaining changes things here; A vorbis file may
     * be a mix of different channels, bitrates and sample rates.
     * You can fetch the information for any section of the file
     * using the ov_ interface.
     */

    while (playback->playing)
    {
        gint current_section = last_section;

        g_mutex_lock (seek_mutex);
        if (seek_value >= 0)
        {
            /* We need to guard against seeking to the end, or things don't
             * work right.  Instead, just seek to one second prior to this.
             */
            if (duration > 0 && seek_value >= duration)
                seek_value = duration - 1;

            playback->output->flush(seek_value * 1000);
            ov_time_seek(&vf, seek_value);
            seek_value = -1;
            g_cond_signal(seek_cond);
        }
        g_mutex_unlock (seek_mutex);


        bytes = ov_read_float(&vf, &pcm, PCM_FRAMES, &current_section);
        if (bytes == OV_HOLE)
            continue;

        if (bytes <= 0)
        {
            while (playback->output->buffer_playing ())
                g_usleep (10000);

            playback->eof = 1;
            break;
        }

        bytes = vorbis_interleave_buffer (pcm, bytes, channels, pcmout);

        if (current_section <= last_section) {
            /*
             * The info struct is different in each section.  vf
             * holds them all for the given bitstream.  This
             * requests the current one
             */
            vi = ov_info(&vf, -1);

            if (vi->channels > 2) {
                playback->eof = TRUE;
                goto stop_processing;
            }

            if (vi->rate != samplerate || vi->channels != channels) {
                samplerate = vi->rate;
                channels = vi->channels;
                while (playback->output->buffer_playing())
                    g_usleep(1000);

                playback->output->close_audio();

                if (!playback->output->open_audio(FMT_FLOAT, vi->rate, vi->channels)) {
                    playback->error = TRUE;
                    playback->eof = TRUE;
                    goto stop_processing;
                }

                playback->output->flush(ov_time_tell(&vf) * 1000);
                vorbis_update_replaygain(&vf, &rg_info);
                playback->set_replaygain_info(playback, &rg_info); /* audio reopened */
            }
        }

        playback->pass_audio(playback, FMT_FLOAT, channels, bytes, pcmout, &playback->playing);

stop_processing:

        if (current_section <= last_section) {
            /*
             * set total play time, bitrate, rate, and channels of
             * current section
             */
            g_free(title);
            title = vorbis_generate_title(&vf, playback->filename);

            if (duration != -1)
                duration = ov_time_total(&vf, -1) * 1000;

            playback->set_params(playback, title, duration, br, samplerate, channels);

            timercount = playback->output->output_time();

            last_section = current_section;

        }
    } /* main loop */

play_cleanup:

    playback->output->close_audio();
    g_free(title);
    ov_clear(&vf);
    playback->playing = 0;
}

static void
vorbis_stop (InputPlayback * playback)
{
    playback->playing = FALSE;
}

static void
vorbis_pause(InputPlayback *playback, gshort p)
{
    playback->output->pause(p);
}

static void
vorbis_seek(InputPlayback *data, gint time)
{
    g_mutex_lock(seek_mutex);
    seek_value = time;
    g_cond_wait(seek_cond, seek_mutex);
    g_mutex_unlock(seek_mutex);
}

static Tuple *
get_song_tuple(const gchar *filename)
{
    VFSFile *stream = NULL;
    OggVorbis_File vfile;          /* avoid thread interaction */
    Tuple *tuple = NULL;
    VFSVorbisFile *fd = NULL;

    if ((stream = aud_vfs_fopen(filename, "rb")) == NULL)
        return NULL;

    fd = g_new0(VFSVorbisFile, 1);
    fd->fd = stream;

    /*
     * The open function performs full stream detection and
     * machine initialization.  If it returns zero, the stream
     * *is* Vorbis and we're fully ready to decode.
     */
    if (ov_open_callbacks(fd, &vfile, NULL, 0, aud_vfs_is_streaming(stream) ? vorbis_callbacks_stream : vorbis_callbacks) < 0)
    {
        aud_vfs_fclose(stream);
        return NULL;
    }

    tuple = get_aud_tuple_for_vorbisfile(&vfile, filename);

    /*
     * once the ov_open succeeds, the stream belongs to
     * vorbisfile.a.  ov_clear will fclose it
     */
    ov_clear(&vfile);

    return tuple;
}

static void
vorbis_aboutbox(void)
{
    static GtkWidget *about_window = NULL;
    if (about_window != NULL)
        gtk_window_present(GTK_WINDOW(about_window));
    else
    {
        about_window = audacious_info_dialog(
        _("About Ogg Vorbis Audio Plugin"),
        /*
         * I18N: UTF-8 Translation: "Haavard Kvaalen" ->
         * "H\303\245vard Kv\303\245len"
         */
        _
        ("Ogg Vorbis Plugin by the Xiph.org Foundation\n\n"
         "Original code by\n"
         "Tony Arcieri <bascule@inferno.tusculum.edu>\n"
         "Contributions from\n"
         "Chris Montgomery <monty@xiph.org>\n"
         "Peter Alm <peter@xmms.org>\n"
         "Michael Smith <msmith@labyrinth.edu.au>\n"
         "Jack Moffitt <jack@icecast.org>\n"
         "Jorn Baayen <jorn@nl.linux.org>\n"
         "Haavard Kvaalen <havardk@xmms.org>\n"
         "Gian-Carlo Pascutto <gcp@sjeng.org>\n"
         "Eugene Zagidullin <e.asphyx@gmail.com>\n\n"
         "Visit the Xiph.org Foundation at http://www.xiph.org/\n"),
        _("Ok"), FALSE, NULL, NULL);
        g_signal_connect(G_OBJECT(about_window), "destroy",
            G_CALLBACK(gtk_widget_destroyed), &about_window);
    }
}

static InputPlugin vorbis_ip;

static void
vorbis_init(void)
{
    mcs_handle_t *db;
    gchar *tmp = NULL;

    memset(&vorbis_cfg, 0, sizeof(vorbis_config_t));
    vorbis_cfg.http_buffer_size = 128;
    vorbis_cfg.http_prebuffer = 25;
    vorbis_cfg.proxy_port = 8080;
    vorbis_cfg.proxy_use_auth = FALSE;
    vorbis_cfg.proxy_user = NULL;
    vorbis_cfg.proxy_pass = NULL;
    vorbis_cfg.tag_override = FALSE;
    vorbis_cfg.tag_format = NULL;

    db = aud_cfg_db_open();
    aud_cfg_db_get_int(db, "vorbis", "http_buffer_size",
                       &vorbis_cfg.http_buffer_size);
    aud_cfg_db_get_int(db, "vorbis", "http_prebuffer",
                       &vorbis_cfg.http_prebuffer);
    aud_cfg_db_get_bool(db, "vorbis", "save_http_stream",
                        &vorbis_cfg.save_http_stream);
    if (!aud_cfg_db_get_string(db, "vorbis", "save_http_path",
                               &vorbis_cfg.save_http_path))
        vorbis_cfg.save_http_path = g_strdup(g_get_home_dir());

    aud_cfg_db_get_bool(db, "vorbis", "tag_override",
                        &vorbis_cfg.tag_override);
    if (!aud_cfg_db_get_string(db, "vorbis", "tag_format",
                               &vorbis_cfg.tag_format))
        vorbis_cfg.tag_format = g_strdup("%p - %t");

    aud_cfg_db_get_bool(db, NULL, "use_proxy", &vorbis_cfg.use_proxy);
    aud_cfg_db_get_string(db, NULL, "proxy_host", &vorbis_cfg.proxy_host);
    aud_cfg_db_get_string(db, NULL, "proxy_port", &tmp);

    if (tmp != NULL)
        vorbis_cfg.proxy_port = atoi(tmp);

    aud_cfg_db_get_bool(db, NULL, "proxy_use_auth", &vorbis_cfg.proxy_use_auth);
    aud_cfg_db_get_string(db, NULL, "proxy_user", &vorbis_cfg.proxy_user);
    aud_cfg_db_get_string(db, NULL, "proxy_pass", &vorbis_cfg.proxy_pass);

    aud_cfg_db_close(db);

    seek_mutex = g_mutex_new();
    seek_cond = g_cond_new();

    aud_mime_set_plugin("application/ogg", &vorbis_ip);
}

static void
vorbis_cleanup(void)
{
    if (vorbis_cfg.save_http_path) {
        g_free(vorbis_cfg.save_http_path);
        vorbis_cfg.save_http_path = NULL;
    }

    if (vorbis_cfg.proxy_host) {
        g_free(vorbis_cfg.proxy_host);
        vorbis_cfg.proxy_host = NULL;
    }

    if (vorbis_cfg.proxy_user) {
        g_free(vorbis_cfg.proxy_user);
        vorbis_cfg.proxy_user = NULL;
    }

    if (vorbis_cfg.proxy_pass) {
        g_free(vorbis_cfg.proxy_pass);
        vorbis_cfg.proxy_pass = NULL;
    }

    if (vorbis_cfg.tag_format) {
        g_free(vorbis_cfg.tag_format);
        vorbis_cfg.tag_format = NULL;
    }

    if (vorbis_cfg.title_encoding) {
        g_free(vorbis_cfg.title_encoding);
        vorbis_cfg.title_encoding = NULL;
    }

    g_strfreev(vorbis_tag_encoding_list);
    g_mutex_free(seek_mutex);
    g_cond_free(seek_cond);
}

static size_t
ovcb_read(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    VFSVorbisFile *handle = (VFSVorbisFile *) datasource;

    return aud_vfs_fread(ptr, size, nmemb, handle->fd);
}

static int
ovcb_seek(void *datasource, int64_t offset, int whence)
{
    VFSVorbisFile *handle = (VFSVorbisFile *) datasource;

    return aud_vfs_fseek(handle->fd, offset, whence);
}

static int
ovcb_close(void *datasource)
{
    VFSVorbisFile *handle = (VFSVorbisFile *) datasource;

    return handle->probe ? 0 : aud_vfs_fclose(handle->fd);
}

static long
ovcb_tell(void *datasource)
{
    VFSVorbisFile *handle = (VFSVorbisFile *) datasource;

    return aud_vfs_ftell(handle->fd);
}


static gchar *vorbis_fmts[] = { "ogg", "ogm", "oga", NULL };

static InputPlugin vorbis_ip = {
    .description = "Ogg Vorbis Audio Plugin",
    .init = vorbis_init,
    .about = vorbis_aboutbox,
    .configure = vorbis_configure,
    .play_file = vorbis_play,
    .stop = vorbis_stop,
    .pause = vorbis_pause,
    .seek = vorbis_seek,
    .cleanup = vorbis_cleanup,
    .get_song_tuple = get_song_tuple,
    .is_our_file_from_vfs = vorbis_check_fd,
    .vfs_extensions = vorbis_fmts,
    .update_song_tuple = vorbis_update_song_tuple,
};

static InputPlugin *vorbis_iplist[] = { &vorbis_ip, NULL };

DECLARE_PLUGIN(vorbis, NULL, NULL, vorbis_iplist, NULL, NULL, NULL, NULL, NULL);

