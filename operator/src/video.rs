//! H.265 access units in, the newest RGBA frame out. decodebin picks the decoder: nvh265dec
//! where NVIDIA's plugin is installed, libde265dec otherwise.

use std::sync::{Arc, Mutex};

use anyhow::Context;
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;

pub struct Frame {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}

pub struct Decoder {
    src: gst_app::AppSrc,
    pipeline: gst::Pipeline,
    pub latest: Arc<Mutex<Option<Frame>>>,
    pub decoded: Arc<Mutex<u64>>,
    /// Sum of push-to-decoded times in ms and their count, since the screen last read them.
    pub decode_sum: Arc<Mutex<(f64, u64)>>,
}

impl Decoder {
    pub fn new() -> anyhow::Result<Self> {
        gst::init()?;
        let pipeline = gst::parse::launch(
            "appsrc name=src is-live=true do-timestamp=true format=time \
             caps=video/x-h265,stream-format=byte-stream,alignment=au \
             ! h265parse ! decodebin ! videoconvert ! video/x-raw,format=RGBA \
             ! appsink name=sink sync=false max-buffers=1 drop=true",
        )?
        .downcast::<gst::Pipeline>()
        .map_err(|_| anyhow::anyhow!("not a pipeline"))?;
        let src = pipeline.by_name("src").context("appsrc")?.downcast::<gst_app::AppSrc>().unwrap();
        let sink = pipeline.by_name("sink").context("appsink")?.downcast::<gst_app::AppSink>().unwrap();
        let latest = Arc::new(Mutex::new(None));
        let decoded = Arc::new(Mutex::new(0));
        let decode_sum = Arc::new(Mutex::new((0.0, 0)));
        let (out, count, dsum) = (latest.clone(), decoded.clone(), decode_sum.clone());
        sink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |sink| {
                    let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    let caps = sample.caps().ok_or(gst::FlowError::Error)?;
                    let info = gstreamer_video::VideoInfo::from_caps(caps).map_err(|_| gst::FlowError::Error)?;
                    let buffer = sample.buffer().ok_or(gst::FlowError::Error)?;
                    // appsrc do-timestamp stamps each access unit with the running time it was pushed at.
                    if let (Some(now), Some(pts)) = (sink.current_running_time(), buffer.pts()) {
                        let mut d = dsum.lock().unwrap();
                        d.0 += now.saturating_sub(pts).nseconds() as f64 / 1e6;
                        d.1 += 1;
                    }
                    let map = buffer.map_readable().map_err(|_| gst::FlowError::Error)?;
                    let (w, h, stride) = (info.width() as usize, info.height() as usize, info.stride()[0] as usize);
                    let mut rgba = Vec::with_capacity(w * h * 4);
                    for row in 0..h {
                        rgba.extend_from_slice(&map[row * stride..row * stride + w * 4]);
                    }
                    *out.lock().unwrap() = Some(Frame { width: w, height: h, rgba });
                    *count.lock().unwrap() += 1;
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );
        pipeline.set_state(gst::State::Playing)?;
        Ok(Decoder { src, pipeline, latest, decoded, decode_sum })
    }

    pub fn push(&self, au: &[u8]) {
        let _ = self.src.push_buffer(gst::Buffer::from_slice(au.to_vec()));
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        let _ = self.pipeline.set_state(gst::State::Null);
    }
}
