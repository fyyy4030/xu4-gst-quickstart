/* Copyright (C) 2014 Tim-Philipp Müller <tim@centricular.com>>
 * Copyright (C) 2014 Centricular Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

/* Test program to demonstrate use of queues to start/stop recording
 * with N seconds backlog on press of a button. Here the backlog is
 * in encoded video. We want to start a new file from a video keyframe
 * (and don't care about dropping some delta frames at the beginning
 * because we have a sufficiently large backlog anyway that the
 * interesting bit will still be in the file).
 *
 * Keeps a backlog of encoded H.264 video in a queue and starts
 * recording on demand (here timeout-based) with the
 * backlog data, whilst making sure the first encoded video
 * frame going into the muxer is a keyframe. This means we
 * drop some video data at the beginning, but it doesn't matter
 * because our backlog is large enough that we always have the
 * interesting bit in the file. Only problem is the audio now,
 * where we should set the backlog to a bit smaller than the
 * video backlog, so that we don't end up with 1-2 seconds of
 * audio before the first video frame.
 */
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <string.h>
#include <stdlib.h>


typedef struct
{
  GstElement *pipeline, *vrecq;
  GstElement *filesink;
  GstElement *muxer;
  GMainLoop *loop;
  GstPad *vrecq_src;
  gulong vrecq_src_probe_id;
  guint buffer_count;
  guint chunk_count;
  SoupServer *server;
  guint stopping;
  guint restart;
  gchar *file_format;
  guint file_modulo;
} RecordApp;

static void start_recording_cb (gpointer user_data);

static void
app_update_filesink_location (RecordApp * app)
{
  gchar *fn;

  fn = g_strdup_printf (app->file_format, app->chunk_count);
  g_print ("Setting filesink location to '%s'\n", fn);
  g_object_set (app->filesink, "location", fn, NULL);

  // Set file location for next run
  app->chunk_count = (app->chunk_count + 1) % app->file_modulo;
  g_free (fn);
}

static gboolean
bus_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  RecordApp *app = user_data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:
      g_printerr ("Error!\n");
      g_main_loop_quit (app->loop);
      return FALSE;
    case GST_MESSAGE_ELEMENT:{
      const GstStructure *s = gst_message_get_structure (msg);

      if (gst_structure_has_name (s, "GstBinForwarded")) {
        GstMessage *forward_msg = NULL;

        gst_structure_get (s, "message", GST_TYPE_MESSAGE, &forward_msg, NULL);
        if (GST_MESSAGE_TYPE (forward_msg) == GST_MESSAGE_EOS) {
          g_print ("EOS from element %s\n",
              GST_OBJECT_NAME (GST_MESSAGE_SRC (forward_msg)));
          gst_element_set_state (app->filesink, GST_STATE_NULL);
          gst_element_set_state (app->muxer, GST_STATE_NULL);
          app_update_filesink_location (app);
          gst_element_set_state (app->filesink, GST_STATE_PLAYING);
          gst_element_set_state (app->muxer, GST_STATE_PLAYING);
          app->stopping = 0;
          if (app->restart == 1) {
            g_print ("restart recording\n");
            start_recording_cb (app);
            app->restart = 0;
          }
        }
        gst_message_unref (forward_msg);
      }
      break;
    }
    default:
      break;
  }

  return TRUE;
}

static GstPadProbeReturn
probe_drop_one_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  RecordApp *app = user_data;
  GstBuffer *buf = info->data;

  if (app->buffer_count++ == 0) {
    g_print ("Drop one buffer with ts %" GST_TIME_FORMAT "\n",
        GST_TIME_ARGS (GST_BUFFER_PTS (info->data)));
    return GST_PAD_PROBE_DROP;
  } else {
    gboolean is_keyframe;

    is_keyframe = !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    g_print ("Buffer with ts %" GST_TIME_FORMAT " (keyframe=%d)\n",
        GST_TIME_ARGS (GST_BUFFER_PTS (buf)), is_keyframe);

    if (is_keyframe) {
      g_print ("Letting buffer through and removing drop probe\n");
      return GST_PAD_PROBE_REMOVE;
    } else {
      g_print ("Dropping buffer, wait for a keyframe.\n");
      return GST_PAD_PROBE_DROP;
    }
  }
}

static gpointer
push_eos_thread (gpointer user_data)
{
  RecordApp *app = user_data;
  GstPad *peer;

  peer = gst_pad_get_peer (app->vrecq_src);
  g_print ("pushing EOS event on pad %s:%s\n", GST_DEBUG_PAD_NAME (peer));

  /* tell pipeline to forward EOS message from filesink immediately and not
   * hold it back until it also got an EOS message from the video sink */
  g_object_set (app->pipeline, "message-forward", TRUE, NULL);

  gst_pad_send_event (peer, gst_event_new_eos ());
  gst_object_unref (peer);

  return NULL;
}

static GstPadProbeReturn
block_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  RecordApp *app = user_data;
  g_print ("pad %s:%s blocked!\n", GST_DEBUG_PAD_NAME (pad));
  g_assert ((info->type & GST_PAD_PROBE_TYPE_BUFFER) ==
      GST_PAD_PROBE_TYPE_BUFFER);
  /* FIXME: this doesn't work: gst_buffer_replace ((GstBuffer **) &info->data, NULL); */

  if(app->stopping == 1) {
    GThread *thread;
    g_print ("Starting eos-push-thread\n");
    thread = g_thread_new ("eos-push-thread", push_eos_thread, app);
    g_thread_unref(thread);
  }

  return GST_PAD_PROBE_OK;
}

static gboolean
stop_recording_cb (gpointer user_data)
{
  RecordApp *app = user_data;

  g_print ("stop recording\n");

  app->vrecq_src_probe_id = gst_pad_add_probe (app->vrecq_src,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, block_probe_cb,
      app, NULL);

  app->stopping = 1;

  return FALSE;                 /* don't call us again */
}

static void
start_recording_cb (gpointer user_data)
{
  RecordApp *app = user_data;

  g_print ("unblocking pad to start recording\n");

  /* need to hook up another probe to drop the initial old buffer stuck
   * in the blocking pad probe */
  app->buffer_count = 0;
  gst_pad_add_probe (app->vrecq_src,
      GST_PAD_PROBE_TYPE_BUFFER, probe_drop_one_cb, app, NULL);

  /* now remove the blocking probe to unblock the pad */
  gst_pad_remove_probe (app->vrecq_src, app->vrecq_src_probe_id);
  app->vrecq_src_probe_id = 0;
}

static void
start_callback (SoupServer        *server,
                 SoupMessage       *msg,
                 const char        *path,
                 GHashTable        *query,
                 SoupClientContext *client,
                 gpointer           user_data)
{
    RecordApp *app = user_data;
    const char *mime_type;
    const char *body;

    g_print ("In start_callback\n");
    if (msg->method != SOUP_METHOD_GET) {
        soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
        return;
    }

    mime_type = "text/html";
    body = "OK";

    if (app->stopping == 0) {
      // only start if we're not busy stopping
      g_print ("Start recording\n");
      start_recording_cb (app);
    } else {
      app->restart = 1;
    }

    g_print ("Sending status OK with body text/html: OK\n");
    soup_message_set_status (msg, SOUP_STATUS_OK);
    soup_message_set_response (msg, mime_type, SOUP_MEMORY_COPY,
                               body, 2);
}

static void
stop_callback (SoupServer        *server,
                 SoupMessage       *msg,
                 const char        *path,
                 GHashTable        *query,
                 SoupClientContext *client,
                 gpointer           user_data)
{
    RecordApp *app = user_data;
    const char *mime_type;
    gchar *loc;

    g_print ("In stop_callback\n");
    if (msg->method != SOUP_METHOD_GET) {
        soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
        return;
    }

    g_object_get (app->filesink, "location", &loc, NULL);

    mime_type = "text/html";

    g_print ("Sop recording\n");
    stop_recording_cb (app);

    g_print ("Sending status OK with body text/html: OK\n");
    soup_message_set_status (msg, SOUP_STATUS_OK);
    soup_message_set_response (msg, mime_type, SOUP_MEMORY_COPY,
                               loc, strlen(loc));
    g_free(loc);
}

int
main (int argc, char **argv)
{
  RecordApp app;
  GError *error = NULL;

  gst_init (NULL, NULL);

  if (argc == 4) {
    app.file_format = argv[1];
    app.file_modulo = atoi(argv[2]);
    app.pipeline = gst_parse_launch (argv[3], NULL);
  } else {
    app.file_format = "/var/tmp/test-%03d.mp4";
    app.file_modulo = 500;
    app.pipeline = gst_parse_launch ("v4l2src device=/dev/video0 do-timestamp=true "
      " ! video/x-raw, format=YUY2,width=1280,height=720,framerate=30/1 "
      " ! v4l2video30convert ! video/x-raw, format=NV12 "
      " ! v4l2video11h264enc extra-controls=encode,h264_level=10,h264_profile=4,frame_level_rate_control_enable=1,video_bitrate=4194304 "
      " ! h264parse config-interval=2 "
      " ! queue name=vrecq ! mp4mux name=mux ! filesink async=false name=filesink",
      NULL);
  }

  app.vrecq = gst_bin_get_by_name (GST_BIN (app.pipeline), "vrecq");
  g_object_set (app.vrecq, "max-size-time", (guint64) 3 * GST_SECOND,
      "max-size-bytes", 0, "max-size-buffers", 0, "leaky", 2, NULL);

  app.vrecq_src = gst_element_get_static_pad (app.vrecq, "src");
  app.vrecq_src_probe_id = gst_pad_add_probe (app.vrecq_src,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, block_probe_cb,
      &app, NULL);

  app.chunk_count = 0;
  app.filesink = gst_bin_get_by_name (GST_BIN (app.pipeline), "filesink");
  app_update_filesink_location (&app);
  app.muxer = gst_bin_get_by_name (GST_BIN (app.pipeline), "mux");

  gst_element_set_state (app.pipeline, GST_STATE_PLAYING);

  app.loop = g_main_loop_new (NULL, FALSE);
  gst_bus_add_watch (GST_ELEMENT_BUS (app.pipeline), bus_cb, &app);

  app.stopping = 0;
  app.restart = 0;
  app.server = soup_server_new (SOUP_SERVER_SERVER_HEADER, "recorder ", NULL);
  soup_server_listen_all (app.server, 9620, 0, &error);
  soup_server_add_handler (app.server, "/start", start_callback, &app, NULL);
  soup_server_add_handler (app.server, "/stop", stop_callback, &app, NULL);
  g_print ("Listening on port 9620...\n");

  g_main_loop_run (app.loop);

  gst_element_set_state (app.pipeline, GST_STATE_NULL);
  gst_object_unref (app.pipeline);
  return 0;
}
