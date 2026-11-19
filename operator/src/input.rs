//! Keyboard and gamepad into one drive command. A key is all or nothing, so held keys are
//! ramped toward their target; a stick already gives a proportional value and is used as is.

use gilrs::{Axis, Button, Gilrs};

pub const MAX_LINEAR_MPS: f64 = 0.6;
pub const MAX_ANGULAR_RPS: f64 = 0.8;
const ACCEL_MPS2: f64 = 1.5;
const DECEL_MPS2: f64 = 3.0;
const ANGULAR_RATE_RPS2: f64 = 2.0;
const STICK_DEADZONE: f32 = 0.08;

#[derive(Clone, Copy, Default)]
pub struct Drive {
    pub linear_mps: f64,
    pub angular_rps: f64,
    pub deadman: bool,
    pub gamepad: bool,
}

pub struct Keys {
    pub forward: bool,
    pub back: bool,
    pub left: bool,
    pub right: bool,
    pub deadman: bool,
    pub focused: bool,
}

pub struct Input {
    gilrs: Option<Gilrs>,
    drive: Drive,
}

fn ramp(current: f64, target: f64, up: f64, down: f64, dt: f64) -> f64 {
    let rate = if target.abs() < current.abs() || target.signum() != current.signum() { down } else { up };
    let step = rate * dt;
    current + (target - current).clamp(-step, step)
}

impl Input {
    pub fn new() -> Self {
        Input { gilrs: Gilrs::new().ok(), drive: Drive::default() }
    }

    pub fn gamepad_name(&self) -> Option<String> {
        let g = self.gilrs.as_ref()?;
        g.gamepads().next().map(|(_, pad)| pad.name().to_string())
    }

    pub fn poll(&mut self, keys: &Keys, dt: f64) -> Drive {
        let dt = dt.min(0.1);
        if let Some(g) = self.gilrs.as_mut() {
            while g.next_event().is_some() {}
            if let Some((_, pad)) = g.gamepads().next() {
                let axis = |a| {
                    let v = pad.value(a);
                    if v.abs() < STICK_DEADZONE { 0.0 } else { v as f64 }
                };
                let deadman = pad.is_pressed(Button::RightTrigger);
                let (lin, ang) = (axis(Axis::LeftStickY), -axis(Axis::RightStickX));
                if deadman || lin != 0.0 || ang != 0.0 {
                    self.drive = Drive {
                        linear_mps: if deadman { lin * MAX_LINEAR_MPS } else { 0.0 },
                        angular_rps: if deadman { ang * MAX_ANGULAR_RPS } else { 0.0 },
                        deadman,
                        gamepad: true,
                    };
                    return self.drive;
                }
            }
        }
        // Without focus every key counts as released: the window gets no key-up it missed.
        let armed = keys.focused && keys.deadman;
        let axis = |pos: bool, neg: bool| (pos as i8 - neg as i8) as f64;
        let (lin_target, ang_target) = if armed {
            (axis(keys.forward, keys.back) * MAX_LINEAR_MPS, axis(keys.left, keys.right) * MAX_ANGULAR_RPS)
        } else {
            (0.0, 0.0)
        };
        self.drive = Drive {
            linear_mps: ramp(self.drive.linear_mps, lin_target, ACCEL_MPS2, DECEL_MPS2, dt),
            angular_rps: ramp(self.drive.angular_rps, ang_target, ANGULAR_RATE_RPS2, ANGULAR_RATE_RPS2, dt),
            deadman: armed,
            gamepad: false,
        };
        self.drive
    }
}

#[cfg(test)]
mod tests {
    use super::ramp;

    #[test]
    fn slowing_down_is_faster_than_speeding_up() {
        assert!((ramp(0.0, 0.6, 1.5, 3.0, 0.1) - 0.15).abs() < 1e-9);
        assert!((ramp(0.6, 0.0, 1.5, 3.0, 0.1) - 0.3).abs() < 1e-9);
    }
}
