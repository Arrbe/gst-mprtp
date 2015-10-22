/* GStreamer
 * Copyright (C) 2013 Collabora Ltd.
 *   @author Torrie Fischer <torrie.fischer@collabora.co.uk>
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
 */
#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


typedef struct _SessionData
{
  int ref;
  guint sessionNum;
  GstElement *input;
} SessionData;

static SessionData *
session_ref (SessionData * data)
{
  g_atomic_int_inc (&data->ref);
  return data;
}

static void
session_unref (gpointer data)
{
  SessionData *session = (SessionData *) data;
  if (g_atomic_int_dec_and_test (&session->ref)) {
    g_free (session);
  }
}

static SessionData *
session_new (guint sessionNum)
{
  SessionData *ret = g_new0 (SessionData, 1);
  ret->sessionNum = sessionNum;
  return session_ref (ret);
}

/*
 * Used to generate informative messages during pipeline startup
 */
static void
cb_state (GstBus * bus, GstMessage * message, gpointer data)
{
  GstObject *pipe = GST_OBJECT (data);
  GstState old, new, pending;
  gst_message_parse_state_changed (message, &old, &new, &pending);
  if (message->src == pipe) {
    g_print ("Pipeline %s changed state from %s to %s\n",
        GST_OBJECT_NAME (message->src),
        gst_element_state_get_name (old), gst_element_state_get_name (new));
  }
}

/*
 * Creates a GstGhostPad named "src" on the given bin, pointed at the "src" pad
 * of the given element
 */
static void
setup_ghost (GstElement * src, GstBin * bin)
{
  GstPad *srcPad = gst_element_get_static_pad (src, "src");
  GstPad *binPad = gst_ghost_pad_new ("src", srcPad);
  gst_element_add_pad (GST_ELEMENT (bin), binPad);
}

static SessionData *
make_audio_session (guint sessionNum)
{
  SessionData *session;
  GstBin *audioBin = GST_BIN (gst_bin_new (NULL));
  GstElement *audioSrc = gst_element_factory_make ("audiotestsrc", NULL);
  GstElement *encoder = gst_element_factory_make ("alawenc", NULL);
  GstElement *payloader = gst_element_factory_make ("rtppcmapay", NULL);
  g_object_set (audioSrc, "is-live", TRUE, NULL);

  gst_bin_add_many (audioBin, audioSrc, encoder, payloader, NULL);
  gst_element_link_many (audioSrc, encoder, payloader, NULL);

  setup_ghost (payloader, audioBin);

  session = session_new (sessionNum);
  session->input = GST_ELEMENT (audioBin);

  return session;
}

static SessionData *
make_video_session (guint sessionNum)
{
  GstBin *videoBin = GST_BIN (gst_bin_new (NULL));
  //GstElement *videoSrc = gst_element_factory_make ("videotestsrc", NULL);
  GstElement *videoSrc = gst_element_factory_make ("autovideosrc", NULL);
  GstElement *videoConv = gst_element_factory_make("videoconvert", NULL);
  GstElement *encoder = gst_element_factory_make ("theoraenc", NULL);
  GstElement *payloader = gst_element_factory_make ("rtptheorapay", NULL);
  GstCaps *videoCaps;
  SessionData *session;
  //g_object_set (videoSrc, "is-live", TRUE, "horizontal-speed", 1, NULL);
  g_object_set (payloader, "config-interval", 2, NULL);
  g_object_set(videoSrc, "name", "videosrc", NULL);
  g_object_set (videoSrc, "sync", FALSE, NULL);
  g_object_set (encoder, "bitrate", 1024, NULL);

  gst_bin_add_many (videoBin, videoSrc, videoConv, encoder, payloader, NULL);
  videoCaps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, 352,
      "height", G_TYPE_INT, 288, "framerate", GST_TYPE_FRACTION, 25, 1, NULL);

  //gst_element_link_filtered (videoSrc, videoConv, videoCaps);
  gst_element_link(videoSrc, videoConv);
  gst_element_link(videoConv, encoder);
  gst_element_link (encoder, payloader);

  //g_object_set(videoSrc, "filter-caps", videoCaps, NULL);

  setup_ghost (payloader, videoBin);

  session = session_new (sessionNum);
  session->input = GST_ELEMENT (videoBin);

  return session;
}

static GstElement *
request_aux_sender (GstElement * rtpbin, guint sessid, SessionData * session)
{
  GstElement *rtx, *bin;
  GstPad *pad;
  gchar *name;
  GstStructure *pt_map;

  GST_INFO ("creating AUX sender");
  bin = gst_bin_new (NULL);
  rtx = gst_element_factory_make ("rtprtxsend", NULL);
  pt_map = gst_structure_new ("application/x-rtp-pt-map",
      "8", G_TYPE_UINT, 98, "96", G_TYPE_UINT, 99, NULL);
  g_object_set (rtx, "payload-type-map", pt_map, NULL);
  gst_structure_free (pt_map);
  gst_bin_add (GST_BIN (bin), rtx);

  pad = gst_element_get_static_pad (rtx, "src");
  name = g_strdup_printf ("src_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  pad = gst_element_get_static_pad (rtx, "sink");
  name = g_strdup_printf ("sink_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  return bin;
}

typedef struct _Identities
{
  GstElement *mprtpsch;
  GstElement *netsim_s1;
  GstElement *netsim_s2;
  guint silence;
  guint called;
  gchar filename[255];
  FILE * fp;
  char * line;
  size_t len;
  ssize_t read;
} Identities;

#define print_boundary(text) g_printf("------------------------------ %s ---------------------\n", text);
#define print_command(str,...) g_printf("[CMD] "str"\n",__VA_ARGS__)

static GstElement *get_netsim_for_subflow(Identities* ids, guint subflow_id)
{
  if(subflow_id == 1)
    return ids->netsim_s1;
  else
    return ids->netsim_s2;
}

static gboolean
_network_changes (gpointer data)
{
  gchar **words, *command;
  Identities *ids = data;

  if(++ids->called == 1){
    ids->fp = fopen(ids->filename, "r");
    g_print("File to open: %s\n", ids->filename);
    if (ids->fp == NULL) goto end;
    print_boundary("START");
  }
  if(ids->silence > 0){
    --ids->silence;
    goto next;
  }

again:
  ids->read = getline(&ids->line, &ids->len, ids->fp);
  if(ids->read == -1) goto end;
  words = g_strsplit(ids->line, ",", 0);
  if(ids->line[0] == '#') goto again;
  command = words[0];

  if(!strcmp(command, "silence")){
    gint times;
    times = atoi(words[1]);
    ids->silence = times;
    print_command("silence will perform for %d times", times);
    goto next;
  }
  if(!strcmp(command, "drop-probability")  ||
     !strcmp(command, "drop-probability")  ||
     !strcmp(command, "delay-probability") ||
     !strcmp(command, "reorder-probability")
    )
  {
    gfloat probability;
    guint subflow_id;
    probability = atof(words[1]);
    subflow_id = atoi(words[2]);
    g_object_set(get_netsim_for_subflow(ids, subflow_id),
                 command, probability, NULL);
    print_command("%s is processed with probability %f on subflow %d",
                  command, probability, subflow_id);
    goto next;
  }


  if(!strcmp(command, "min-delay")     ||
      !strcmp(command, "max-delay")    ||
      !strcmp(command, "drop-packets") ||
      !strcmp(command, "bandwidth")){
    gint value;
    guint subflow_id;
    value = atoi(words[1]);
    subflow_id = atoi(words[2]);
    g_object_set(get_netsim_for_subflow(ids, subflow_id),
                 command, value, NULL);
    print_command("%s is processed with value %d on subflow %d",
                  command, value, subflow_id);
    goto next;
  }

next:
  return G_SOURCE_CONTINUE;

end:
  if(ids->fp) fclose(ids->fp);
  if(ids->line) free(ids->line);
  print_boundary("END");
  return G_SOURCE_REMOVE;
}


static void
add_stream (GstPipeline * pipe, GstElement * rtpBin, SessionData * session,
           gchar* file)
{

  GstElement *rtpSink_1 = gst_element_factory_make ("udpsink", NULL);
  GstElement *rtpSink_2 = gst_element_factory_make ("udpsink", NULL);
  GstElement *rtcpSink = gst_element_factory_make ("udpsink", NULL);
  GstElement *rtcpSrc = gst_element_factory_make ("udpsrc", NULL);
  GstElement *rtpSrc_1 = gst_element_factory_make ("udpsrc", NULL);
  GstElement *rtpSrc_2 = gst_element_factory_make ("udpsrc", NULL);
  GstElement *mprtpsnd = gst_element_factory_make ("mprtpsender", NULL);
  GstElement *mprtprcv = gst_element_factory_make ("mprtpreceiver", NULL);
  GstElement *mprtpsch = gst_element_factory_make ("mprtpscheduler", NULL);
  GstElement *identity_1 = gst_element_factory_make ("netsim", NULL);
  GstElement *identity_2 = gst_element_factory_make ("netsim", NULL);
  Identities *ids = g_malloc0 (sizeof (Identities));
  int basePort;
  gchar *padName;

  ids->mprtpsch = mprtpsch;
  ids->netsim_s1 = identity_1;
  ids->netsim_s2 = identity_2;
  ids->called = 0;

  basePort = 5000 + (session->sessionNum * 20);

  gst_bin_add_many (GST_BIN (pipe), rtpSink_1, rtpSink_2, mprtprcv, mprtpsnd,
      mprtpsch, rtcpSink, rtcpSrc, rtpSrc_1, rtpSrc_2, identity_1,
      identity_2, session->input, NULL);

  /* enable retransmission by setting rtprtxsend as the "aux" element of rtpbin */
  g_signal_connect (rtpBin, "request-aux-sender",
      (GCallback) request_aux_sender, session);

  g_object_set (rtpSink_1, "port", basePort, "host", "127.0.0.1",
      NULL);
//      "sync",FALSE, "async", FALSE, NULL);

  g_object_set (rtpSink_2, "port", basePort + 1, "host", "127.0.0.1",
//      NULL);
      "sync",FALSE, "async", FALSE, NULL);

  g_object_set (rtcpSink, "port", basePort + 5, "host", "127.0.0.1",
//      NULL);
       "sync",FALSE, "async", FALSE, NULL);

  g_object_set (rtpSrc_1, "port", basePort + 11, NULL);
  g_object_set (rtpSrc_2, "port", basePort + 12, NULL);
  g_object_set (rtcpSrc, "port", basePort + 10, NULL);
  g_object_set (mprtpsch, "auto-flow-controlling", TRUE, NULL);

  padName = g_strdup_printf ("send_rtp_sink_%u", session->sessionNum);
  gst_element_link_pads (session->input, "src", rtpBin, padName);
  g_free (padName);

  //MPRTP Sender
  padName = g_strdup_printf ("send_rtp_src_%u", session->sessionNum);
  gst_element_link_pads (rtpBin, padName, mprtpsch, "rtp_sink");
  gst_element_link_pads (mprtpsch, "mprtp_src", mprtpsnd, "mprtp_sink");
  g_free (padName);
  /* link rtpbin to udpsink directly here if you don't want
   * artificial packet loss */
  gst_element_link_pads (mprtpsnd, "src_1", identity_1, "sink");
  gst_element_link (identity_1, rtpSink_1);

  gst_element_link_pads (mprtpsnd, "src_2", identity_2, "sink");
  gst_element_link (identity_2, rtpSink_2);

  g_object_set (mprtpsch, "join-subflow", 1, NULL);
  g_object_set (mprtpsch, "join-subflow", 2, NULL);

  sprintf(ids->filename, "%s", file);
  g_timeout_add (1000, _network_changes, ids);

  padName = g_strdup_printf ("send_rtcp_src_%u", session->sessionNum);
  gst_element_link_pads (rtpBin, padName, rtcpSink, "sink");
  g_free (padName);


  padName = g_strdup_printf ("recv_rtcp_sink_%u", session->sessionNum);
  gst_element_link_pads (rtpSrc_1, "src", mprtprcv, "sink_1");
  gst_element_link_pads (rtpSrc_2, "src", mprtprcv, "sink_2");
  //gst_element_link_pads(mprtprecv, "rtcp_src", rtpBin, padName);
  gst_element_link_pads (mprtprcv, "mprtcp_rr_src", mprtpsch, "mprtcp_rr_sink");
  gst_element_link_pads (mprtpsch, "mprtcp_sr_src", mprtpsnd, "mprtcp_sr_sink");
  gst_element_link_pads (rtcpSrc, "src", rtpBin, padName);
  g_free (padName);

  g_print ("New RTP stream on %i/%i/%i\n", basePort, basePort + 1,
      basePort + 5);

  session_unref (session);
}

int
main (int argc, char **argv)
{
  GstPipeline *pipe;
  GstBus *bus;
  SessionData *videoSession;
  SessionData *audioSession;
  GstElement *rtpBin;
  GMainLoop *loop;
  gchar *testfile = NULL;

  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  pipe = GST_PIPELINE (gst_pipeline_new (NULL));
  bus = gst_element_get_bus (GST_ELEMENT (pipe));
  g_signal_connect (bus, "message::state-changed", G_CALLBACK (cb_state), pipe);
  gst_bus_add_signal_watch (bus);
  gst_object_unref (bus);

  rtpBin = gst_element_factory_make ("rtpbin", NULL);
  g_object_set (rtpBin, "rtp-profile", GST_RTP_PROFILE_AVPF, NULL);

  gst_bin_add (GST_BIN (pipe), rtpBin);

  if(argc > 1) testfile = argv[1];

  videoSession = make_video_session (0);
  add_stream (pipe, rtpBin, videoSession, testfile);

  g_print ("starting server pipeline\n");
  gst_element_set_state (GST_ELEMENT (pipe), GST_STATE_PLAYING);

  g_main_loop_run (loop);

  g_print ("stopping server pipeline\n");
  gst_element_set_state (GST_ELEMENT (pipe), GST_STATE_NULL);

  gst_object_unref (pipe);
  g_main_loop_unref (loop);

  return 0;
}