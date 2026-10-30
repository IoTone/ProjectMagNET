// SPDX-License-Identifier: Apache-2.0
`timescale 1ns/1ps
//
// sweep -- walk an arc, stopping at each station, and tell the host when to
// take a sample. This is PLAN.md T5a / section 4.6.6.
//
// The host writes start, end, step and dwell ONCE and pulses `go`. The fabric
// then does the whole scan: move, settle, strobe, move, settle, strobe. The
// Artemis reads the OPT4048 on each `strobe` and never commands a step, which
// removes 16-60 UART round-trips per scan and all of their latency variance.
//
// ORDER MATTERS: arrive, THEN dwell, THEN strobe. The OPT4048 integrates
// continuously over its conversion window, so a reading taken the instant the
// motor stops is smeared across the arc it just crossed. The dwell must cover a
// whole conversion that began after the axis was already stationary, which
// means
//
//     dwell_ms >= 2 x conversion_ms
//
// -- 220 ms for the 100 ms conversion the driver configures, not 100 ms. One
// conversion time is the tempting number and it is wrong: it only guarantees
// that A conversion finished, not that the one you read started after the
// motion stopped. (With the OPT4048's INT line wired to the FPGA this becomes
// "strobe on the second conversion-done after arrival" and the dwell can go
// away -- the POS_LATCH path of D13. Not this module.)
//
// `station_pos` is driven at the axis as an absolute target, so every sample is
// tagged with an exact position. Nothing here dead-reckons.
module sweep #(
	parameter CLK_HZ      = 50_000_000,
	parameter MAX_STATION = 4096		/* runaway guard */
) (
	input  wire               clk,
	input  wire               rst,

	/* SWEEP_CFG */
	input  wire signed [31:0] cfg_start,	/* half-steps, absolute */
	input  wire signed [31:0] cfg_end,
	input  wire        [31:0] cfg_step,	/* magnitude, > 0 */
	input  wire        [31:0] cfg_dwell_ms,

	/* SWEEP_CTL */
	input  wire               go,
	input  wire               abort,

	output reg                busy,
	output reg                done,		/* sticky: completed the arc */
	output reg                aborted,	/* sticky: aborted or refused */
	output reg         [15:0] station,	/* index, 0-based */
	output reg                strobe,	/* 1 clock: sample NOW */
	output reg  signed [31:0] pos_latch,	/* position at the last strobe */

	/* to motion_axis */
	output reg  signed [31:0] m_target,
	output reg                m_go,
	output reg                m_stop,
	input  wire               m_moving,
	input  wire               m_at_target,
	input  wire signed [31:0] m_pos
);
	localparam S_IDLE   = 3'd0;
	localparam S_ISSUE  = 3'd1;	/* target presented, pulse m_go */
	localparam S_SEEK   = 3'd2;	/* wait for the axis to arrive */
	localparam S_DWELL  = 3'd3;	/* stationary, wait out the conversions */
	localparam S_STROBE = 3'd4;	/* latch position, tell the host to read */
	localparam S_NEXT   = 3'd5;	/* advance or finish */
	localparam S_ABORT  = 3'd6;	/* wait for the axis to stop */

	localparam MS_DIV = CLK_HZ / 1000;

	reg [2:0]  state;
	reg [31:0] mscnt;
	reg [31:0] msleft;

	/* Direction of travel along the arc, and the next station. */
	wire               up    = (cfg_end >= cfg_start);
	wire signed [31:0] nxt   = up ? m_target + $signed({1'b0, cfg_step[30:0]})
	                              : m_target - $signed({1'b0, cfg_step[30:0]});
	/* Stop before overshooting `end`: a partial step at the far end is not a
	 * station. The last station is the last one still inside the arc. */
	wire               more  = up ? (nxt <= cfg_end) : (nxt >= cfg_end);
	wire               room  = (station < MAX_STATION - 1);

	always @(posedge clk) begin
		strobe <= 1'b0;
		m_go   <= 1'b0;
		m_stop <= 1'b0;

		if (rst) begin
			state     <= S_IDLE;
			busy      <= 1'b0;
			done      <= 1'b0;
			aborted   <= 1'b0;
			station   <= 16'd0;
			pos_latch <= 32'sd0;
			m_target  <= 32'sd0;
			mscnt     <= 32'd0;
			msleft    <= 32'd0;
		end else if (abort && state != S_IDLE) begin
			m_stop  <= 1'b1;
			state   <= S_ABORT;
		end else begin
			case (state)
			S_IDLE: begin
				busy <= 1'b0;
				if (go) begin
					done    <= 1'b0;
					aborted <= 1'b0;
					station <= 16'd0;
					if (cfg_step == 32'd0) begin
						/* A zero step would scan forever. Refuse it
						 * here rather than hang the axis. */
						aborted <= 1'b1;
						done    <= 1'b1;
					end else begin
						busy     <= 1'b1;
						m_target <= cfg_start;
						state    <= S_ISSUE;
					end
				end
			end

			S_ISSUE: begin
				m_go  <= 1'b1;
				state <= S_SEEK;
			end

			S_SEEK: begin
				/* Covers the zero-distance case too: if the axis was
				 * already there it never asserts m_moving, and
				 * m_at_target is already true. */
				if (!m_moving && m_at_target) begin
					msleft <= cfg_dwell_ms;
					mscnt  <= 32'd0;
					state  <= S_DWELL;
				end
			end

			S_DWELL: begin
				if (msleft == 32'd0) begin
					state <= S_STROBE;
				end else if (mscnt == MS_DIV - 1) begin
					mscnt  <= 32'd0;
					msleft <= msleft - 32'd1;
				end else begin
					mscnt <= mscnt + 32'd1;
				end
			end

			S_STROBE: begin
				pos_latch <= m_pos;
				strobe    <= 1'b1;
				state     <= S_NEXT;
			end

			S_NEXT: begin
				if (more && room) begin
					m_target <= nxt;
					station  <= station + 16'd1;
					state    <= S_ISSUE;
				end else begin
					done    <= 1'b1;
					aborted <= !room;	/* ran into the guard */
					busy    <= 1'b0;
					state   <= S_IDLE;
				end
			end

			S_ABORT: begin
				if (!m_moving) begin
					aborted <= 1'b1;
					busy    <= 1'b0;
					state   <= S_IDLE;
				end
			end

			default: state <= S_IDLE;
			endcase
		end
	end
endmodule
