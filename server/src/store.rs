//! One MCAP file per vehicle and UTC hour, with the CDR messages as the vehicle's ROS topics, so
//! `ros2 bag play` reads it back. The schemas carry only the type name: rosbag2 finds the type
//! support by that name.

use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::BufWriter;
use std::path::PathBuf;

use anyhow::Context;

pub struct Store {
    dir: PathBuf,
    hour: Option<String>,
    writer: Option<mcap::Writer<BufWriter<File>>>,
    channels: HashMap<String, u16>,
    sequence: u32,
}

/// UTC "YYYYMMDD-HH" of a Unix time, by the civil-from-days algorithm (days since 1970-01-01).
pub fn utc_hour(unix_ns: u64) -> String {
    let secs = unix_ns / 1_000_000_000;
    let z = (secs / 86_400) as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}{month:02}{day:02}-{:02}", secs % 86_400 / 3_600)
}

impl Store {
    pub fn new(dir: PathBuf) -> Self {
        Store { dir, hour: None, writer: None, channels: HashMap::new(), sequence: 0 }
    }

    pub fn write(&mut self, topic: &str, msg_type: &str, log_ns: u64, cdr: &[u8]) -> anyhow::Result<()> {
        let hour = utc_hour(log_ns);
        if self.hour.as_deref() != Some(hour.as_str()) {
            self.finish()?;
            std::fs::create_dir_all(&self.dir).with_context(|| format!("{}", self.dir.display()))?;
            let path = self.dir.join(format!("{hour}.mcap"));
            // A restart within the hour must not overwrite what was written before it.
            let path = (1..).map(|n| if n == 1 { path.clone() } else { self.dir.join(format!("{hour}_{n}.mcap")) })
                .find(|p| !p.exists())
                .unwrap();
            self.writer = Some(mcap::Writer::new(BufWriter::new(File::create(&path)?))?);
            self.hour = Some(hour);
        }
        let writer = self.writer.as_mut().unwrap();
        let channel = match self.channels.get(topic) {
            Some(c) => *c,
            None => {
                let schema = writer.add_schema(msg_type, "ros2msg", &[])?;
                let c = writer.add_channel(schema, topic, "cdr", &BTreeMap::new())?;
                self.channels.insert(topic.to_string(), c);
                c
            }
        };
        self.sequence = self.sequence.wrapping_add(1);
        writer.write_to_known_channel(
            &mcap::records::MessageHeader { channel_id: channel, sequence: self.sequence, log_time: log_ns, publish_time: log_ns },
            cdr,
        )?;
        Ok(())
    }

    pub fn finish(&mut self) -> anyhow::Result<()> {
        if let Some(mut w) = self.writer.take() {
            w.finish()?;
        }
        self.channels.clear();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utc_hours() {
        assert_eq!(utc_hour(0), "19700101-00");
        // 2026-11-12 10:30:00 UTC
        assert_eq!(utc_hour(1_794_479_400 * 1_000_000_000), "20261112-10");
        // 2024-02-29 23:59:59 UTC, a leap day
        assert_eq!(utc_hour(1_709_251_199 * 1_000_000_000), "20240229-23");
    }

    #[test]
    fn writes_a_readable_file_and_keeps_an_earlier_one() {
        let dir = std::env::temp_dir().join(format!("fleetd-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for _ in 0..2 {
            let mut s = Store::new(dir.clone());
            s.write("/odom", "nav_msgs/msg/Odometry", 1_794_479_400 * 1_000_000_000, &[0, 1, 0, 0, 7]).unwrap();
            s.finish().unwrap();
        }
        let first = std::fs::read(dir.join("20261112-10.mcap")).unwrap();
        assert!(dir.join("20261112-10_2.mcap").exists());
        let messages: Vec<_> = mcap::MessageStream::new(&first).unwrap().collect::<Result<_, _>>().unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].channel.topic, "/odom");
        assert_eq!(messages[0].channel.schema.as_ref().unwrap().name, "nav_msgs/msg/Odometry");
        assert_eq!(&messages[0].data[..], &[0, 1, 0, 0, 7]);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
