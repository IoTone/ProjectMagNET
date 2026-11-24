// SPDX-License-Identifier: Apache-2.0
`timescale 1ns/1ps
//
// step_gen -- constant-rate step pulse generator.
//
// A modulo-CLK_HZ accumulator, so `vel` is EXACTLY steps per second with no
// scale factor, no divider and no lookup: add vel every clock, and every time
// the accumulator passes CLK_HZ, emit one step and subtract CLK_HZ. The long-run
// rate is vel steps/s with zero cumulative error; the only error is the +/-1
// clock (20 ns at 50 MHz) of where each individual edge lands.
//
// Constraint: vel <= CLK_HZ. The planner clamps it; this module does not.
module step_gen #(
	parameter CLK_HZ = 50_000_000
) (
	input  wire        clk,
	input  wire        rst,
	input  wire        en,
	input  wire [31:0] vel,	/* steps/s; 0 = stopped */
	output reg         step	/* 1-clock pulse per step */
);
	reg  [31:0] acc;
	wire [31:0] nxt = acc + vel;

	always @(posedge clk) begin
		if (rst || !en) begin
			acc  <= 32'd0;
			step <= 1'b0;
		end else if (nxt >= CLK_HZ) begin
			acc  <= nxt - CLK_HZ;
			step <= 1'b1;
		end else begin
			acc  <= nxt;
			step <= 1'b0;
		end
	end
endmodule
