/*
 *  Copyright © 2017-2020 Wellington Wallace
 *
 *  This file is part of PulseEffects.
 *
 *  PulseEffects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  PulseEffects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with PulseEffects.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "plugin_base.hpp"
#include "gst/gstelement.h"
#include "util.hpp"

namespace {

void on_state_changed(GSettings* settings, gchar* key, PluginBase* l) {
  if (l->plugin_is_installed) {
    int enable = g_settings_get_boolean(settings, key);

    if (enable == 1) {
      l->enable();
    } else {
      l->disable();
    }
  } else {
    g_settings_set_boolean(settings, "installed", 0);
  }
}

void on_enable(gpointer user_data) {
  auto* l = static_cast<PluginBase*>(user_data);

  auto* b = gst_bin_get_by_name(GST_BIN(l->plugin), std::string(l->name + "_bin").c_str());

  if (b == nullptr) {
    gst_element_set_state(l->bin, GST_STATE_NULL);

    gst_element_unlink(l->identity_in, l->identity_out);

    gst_bin_add(GST_BIN(l->plugin), l->bin);

    gst_element_link_many(l->identity_in, l->bin, l->identity_out, nullptr);

    gst_element_sync_state_with_parent(l->bin);

    util::debug(l->log_tag + l->name + " is enabled");
  } else {
    util::debug(l->log_tag + l->name + " is already enabled");
  }
}

void on_disable(gpointer user_data) {
  auto* l = static_cast<PluginBase*>(user_data);

  auto* b = gst_bin_get_by_name(GST_BIN(l->plugin), std::string(l->name + "_bin").c_str());

  if (b != nullptr) {
    gst_element_set_state(l->bin, GST_STATE_NULL);

    gst_element_unlink_many(l->identity_in, l->bin, l->identity_out, nullptr);

    gst_bin_remove(GST_BIN(l->plugin), l->bin);

    gst_element_link(l->identity_in, l->identity_out);

    util::debug(l->log_tag + l->name + " is disabled");
  } else {
    util::debug(l->log_tag + l->name + " is already disabled");
  }
}

auto event_probe_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
  if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_CUSTOM_DOWNSTREAM) {
    return GST_PAD_PROBE_PASS;
  }

  gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

  on_disable(user_data);

  return GST_PAD_PROBE_DROP;
}

auto on_pad_blocked(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
  auto* l = static_cast<PluginBase*>(user_data);

  gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

  auto* srcpad = gst_element_get_static_pad(l->identity_out, "src");

  gst_pad_add_probe(srcpad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                    event_probe_cb, user_data, nullptr);

  auto* sinkpad = gst_element_get_static_pad(l->bin, "sink");

  GstStructure* s = gst_structure_new_empty("remove_plugin");

  GstEvent* event = gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, s);

  gst_pad_send_event(sinkpad, event);

  gst_object_unref(sinkpad);
  gst_object_unref(srcpad);

  return GST_PAD_PROBE_OK;
}

}  // namespace

PluginBase::PluginBase(std::string tag,
                       std::string plugin_name,
                       const std::string& schema,
                       const std::string& schema_path)
    : log_tag(std::move(tag)),
      name(std::move(plugin_name)),
      settings(g_settings_new_with_path(schema.c_str(), schema_path.c_str())) {
  plugin = gst_bin_new(std::string(name + "_plugin").c_str());
  identity_in = gst_element_factory_make("identity", std::string(name + "_plugin_bin_identity_in").c_str());
  identity_out = gst_element_factory_make("identity", std::string(name + "_plugin_bin_identity_out").c_str());

  gst_bin_add_many(GST_BIN(plugin), identity_in, identity_out, nullptr);
  gst_element_link_many(identity_in, identity_out, nullptr);

  auto* sinkpad = gst_element_get_static_pad(identity_in, "sink");
  auto* srcpad = gst_element_get_static_pad(identity_out, "src");

  gst_element_add_pad(plugin, gst_ghost_pad_new("sink", sinkpad));
  gst_element_add_pad(plugin, gst_ghost_pad_new("src", srcpad));

  g_object_unref(sinkpad);
  g_object_unref(srcpad);

  bin = gst_bin_new((name + "_bin").c_str());

  on_state_changed(settings, "state", this);
}

PluginBase::~PluginBase() {
  auto enable = g_settings_get_boolean(settings, "state");

  if (enable == false) {
    gst_object_unref(bin);
  }

  g_object_unref(settings);
}

auto PluginBase::is_installed(GstElement* e) -> bool {
  if (e != nullptr) {
    plugin_is_installed = true;

    g_settings_set_boolean(settings, "installed", 1);

    g_signal_connect(settings, "changed::state", G_CALLBACK(on_state_changed), this);

    return true;
  }

  plugin_is_installed = false;

  g_settings_set_boolean(settings, "installed", 0);

  util::warning(name + " plugin was not found!");

  return false;
}

void PluginBase::enable() {
  auto* srcpad = gst_element_get_static_pad(identity_in, "src");

  gst_pad_add_probe(
      srcpad, GST_PAD_PROBE_TYPE_IDLE,
      [](auto pad, auto info, auto d) {
        on_enable(d);

        return GST_PAD_PROBE_REMOVE;
      },
      this, nullptr);

  g_object_unref(srcpad);
}

void PluginBase::disable() {
  auto* srcpad = gst_element_get_static_pad(identity_in, "src");

  GstState state = GST_STATE_NULL;
  GstState pending = GST_STATE_NULL;

  gst_element_get_state(bin, &state, &pending, 0);

  if (state != GST_STATE_PLAYING) {
    gst_pad_add_probe(
        srcpad, GST_PAD_PROBE_TYPE_IDLE,
        [](auto pad, auto info, auto d) {
          on_disable(d);

          return GST_PAD_PROBE_REMOVE;
        },
        this, nullptr);
  } else {
    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, on_pad_blocked, this, nullptr);
  }

  g_object_unref(srcpad);
}
