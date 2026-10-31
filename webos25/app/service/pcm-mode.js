/* Experimental C5 PCM policy. Commands are author constants; no stream URI or
 * caller-supplied path is interpolated. Compatible with old service runtimes. */
"use strict";

function steps(on, p) {
  var helper = p.dest + "/pcm-mat-watch.py";
  var marker = p.dest + "/pcm-multichannel";
  var lines = [p.appbase, "set -eu"];
  if (on) {
    lines.push("command -v python3 >/dev/null || { echo 'FAIL: Python 3 is required'; exit 1; }");
    lines.push('python3 "' + p.payload + '/pcm-mat-watch.py" --check-idle');
    lines.push("GST_REGISTRY_UPDATE=no GST_REGISTRY_1_0=/mnt/flash/data/gst_1_0_registry.arm.bin GST_REGISTRY_FORK=no gst-inspect-1.0 dtsdec | grep -q '^  pcm-multichannel' || { echo 'FAIL: Press Enable first to install this decoder'; exit 1; }");
    lines.push('[ ! -f "' + helper + '" ] || python3 "' + helper + '" --stop');
    lines.push('cp -f "' + p.payload + '/pcm-mat-watch.py" "' + helper + '"');
    lines.push('rm -f "' + p.dest + '/passthrough"');
    lines.push('W25_PT=stop sh "' + p.init + '"');
    lines.push('touch "' + marker + '"');
    lines.push('nohup python3 "' + helper + '" </dev/null >/dev/null 2>&1 &');
  } else {
    lines.push('rm -f "' + marker + '"');
    lines.push('[ ! -f "' + helper + '" ] || python3 "' + helper + '" --stop');
  }
  lines.push("echo OK");
  return lines.join("\n");
}

function register(service, h) {
  service.register("setPcmMultichannel", function (message) {
    if (!message.payload || typeof message.payload.enabled !== "boolean") {
      message.respond({returnValue: false, errorText: "enabled must be a boolean"});
      return;
    }
    var want = message.payload.enabled;
    h.detect().then(function (d) {
      if (d.profile !== h.profile) throw new Error("PCM mode requires the webOS 25 profile");
      return h.exec(steps(want, h.paths));
    }).then(function () {
      message.respond({returnValue: true, enabled: want,
        summary: want ? "Experimental PCM 5.1 enabled. Restart the video. Requires eARC and Digital Sound Out Auto."
          : "PCM mode off; the normal TV sound policy is restored."});
    }).catch(function (e) {
      message.respond({returnValue: false, errorText: e.errorText || e.message || "PCM mode failed"});
    });
  });
}

module.exports = {register: register, steps: steps};
