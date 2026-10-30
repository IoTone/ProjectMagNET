// SPDX-License-Identifier: Apache-2.0
`timescale 1ns/1ps
//
// motion_axis -- one pan/tilt axis: trapezoidal planner, position counter,
// limit and e-stop gating, ULN2003 phase outputs.
//
// Everything is in half-steps. Position is a signed count, so it is exact by
// construction -- there is nothing to drift and nothing to calibrate.
//
// The planner is integer-only: no multiply, no divide, no square root.
//
//   Velocity    `vel` in steps/s, fed straight to step_gen.
//   Ramp clock  a tick at CLK_HZ/1024, so one tick is ~1/1024 s and the
//               velocity delta per tick is accel >> 10 -- a shift, not a
//               divide. The low 10 bits of accel are carried in `facc` so the
//               average acceleration is exact, not truncated.
//   Decel point counted, not computed. `n_acc` counts the steps taken while
//               accelerating; deceleration starts when the steps remaining
//               fall to n_acc. For a symmetric ramp that is exactly right, and
//               it avoids the v^2/2a that would otherwise want a divider.
//
// VMIN is the start/stop velocity. A stepper cannot usefully ramp from zero --
// and a profile that asymptotes to zero velocity never arrives -- so the ramp
// runs between VMIN and vmax and the last VMIN steps/s are simply dropped.
module motion_axis #(
	parameter CLK_HZ = 50_000_000,
	parameter VMIN   = 100		/* steps/s */
) (
	input  wire               clk,
	input  wire               rst,

	input  wire               enable,	/* coil power */
	input  wire signed [31:0] target,	/* half-steps, absolute */
	input  wire               go,		/* pulse: move to target */
	input  wire               stop,		/* pulse: ramped stop, wherever */
	input  wire               estop,	/* LEVEL: halt this clock */
	input  wire        [31:0] vmax,		/* steps/s */
	input  wire        [31:0] accel,	/* steps/s^2 */

	input  wire               lim_cw,	/* active high, polarity already fixed */
	input  wire               lim_ccw,

	output reg  signed [31:0] pos,
	output reg                moving,
	output wire               at_target,
	output reg                lim_hit,
	output reg                estop_hit,
	output wire        [3:0]  phase,
	output wire               step_pulse
);
	localparam ST_IDLE   = 3'd0;
	localparam ST_ACCEL  = 3'd1;
	localparam ST_CRUISE = 3'd2;
	localparam ST_DECEL  = 3'd3;	/* decelerating into the target */
	localparam ST_HALT   = 3'd4;	/* decelerating to a stop, target ignored */

	localparam TICK_DIV = CLK_HZ / 1024;

	reg  [2:0]  state;
	reg  [31:0] vel;
	reg         dir;		/* 1 = increasing position */
	reg  [31:0] n_acc;
	reg  [31:0] tickcnt;
	reg  [9:0]  facc;

	wire signed [31:0] delta = target - pos;
	wire        [31:0] rem   = (delta < 0) ? -delta : delta;

	/* Ramp tick and the exact-average velocity delta. */
	wire        tick  = (tickcnt == TICK_DIV - 1);
	wire [10:0] fsum  = {1'b0, facc} + {1'b0, accel[9:0]};
	wire [31:0] dv    = (accel >> 10) + {31'd0, fsum[10]};

	/* Gating. A limit only blocks motion INTO it, so you can always back off. */
	wire blocked = (dir && lim_cw) || (!dir && lim_ccw);
	wire run     = (state != ST_IDLE) && !estop && !blocked && enable;

	wire step_raw;
	step_gen #(.CLK_HZ(CLK_HZ)) u_sg (
		.clk(clk), .rst(rst), .en(run), .vel(vel), .step(step_raw)
	);
	wire step_eff = step_raw & run;

	phase_seq u_ph (
		.clk(clk), .rst(rst), .en(enable), .step(step_eff), .dir(dir),
		.phase(phase)
	);

	assign step_pulse = step_eff;
	assign at_target  = (pos == target) && !moving;

	always @(posedge clk) begin
		if (rst) begin
			state     <= ST_IDLE;
			vel       <= 32'd0;
			dir       <= 1'b1;
			pos       <= 32'sd0;
			n_acc     <= 32'd0;
			tickcnt   <= 32'd0;
			facc      <= 10'd0;
			moving    <= 1'b0;
			lim_hit   <= 1'b0;
			estop_hit <= 1'b0;
		end else begin
			tickcnt <= tick ? 32'd0 : tickcnt + 32'd1;
			if (tick)
				facc <= fsum[9:0];

			if (step_eff)
				pos <= dir ? pos + 32'sd1 : pos - 32'sd1;

			if (estop) begin
				/* One clock, no planner involvement, no CPU in the path. */
				state     <= ST_IDLE;
				vel       <= 32'd0;
				moving    <= 1'b0;
				estop_hit <= 1'b1;
			end else if (state != ST_IDLE && blocked) begin
				state   <= ST_IDLE;
				vel     <= 32'd0;
				moving  <= 1'b0;
				lim_hit <= 1'b1;
			end else begin
				case (state)
				ST_IDLE: begin
					vel <= 32'd0;
					if (go && enable && rem != 32'd0) begin
						dir       <= (delta > 0);
						n_acc     <= 32'd0;
						vel       <= (vmax < VMIN) ? vmax : VMIN;
						state     <= ST_ACCEL;
						moving    <= 1'b1;
						lim_hit   <= 1'b0;
						estop_hit <= 1'b0;
					end else begin
						moving <= 1'b0;
					end
				end

				ST_ACCEL: begin
					if (step_eff)
						n_acc <= n_acc + 32'd1;

					if (rem == 32'd0 || (rem == 32'd1 && step_eff)) begin
						state  <= ST_IDLE;
						vel    <= 32'd0;
						moving <= 1'b0;
					end else if (stop) begin
						state <= ST_HALT;
					end else if (rem <= n_acc) begin
						state <= ST_DECEL;
					end else if (tick) begin
						if (vel + dv >= vmax) begin
							vel   <= vmax;
							state <= ST_CRUISE;
						end else begin
							vel <= vel + dv;
						end
					end
				end

				ST_CRUISE: begin
					if (rem == 32'd0 || (rem == 32'd1 && step_eff)) begin
						state  <= ST_IDLE;
						vel    <= 32'd0;
						moving <= 1'b0;
					end else if (stop) begin
						state <= ST_HALT;
					end else if (rem <= n_acc) begin
						state <= ST_DECEL;
					end
				end

				ST_DECEL: begin
					if (rem == 32'd0 || (rem == 32'd1 && step_eff)) begin
						state  <= ST_IDLE;
						vel    <= 32'd0;
						moving <= 1'b0;
					end else if (stop) begin
						state <= ST_HALT;
					end else if (tick) begin
						vel <= (vel > VMIN + dv) ? vel - dv : VMIN;
					end
				end

				ST_HALT: begin
					if (tick) begin
						if (vel > VMIN + dv) begin
							vel <= vel - dv;
						end else begin
							state  <= ST_IDLE;
							vel    <= 32'd0;
							moving <= 1'b0;
						end
					end
				end

				default: state <= ST_IDLE;
				endcase
			end
		end
	end
endmodule
