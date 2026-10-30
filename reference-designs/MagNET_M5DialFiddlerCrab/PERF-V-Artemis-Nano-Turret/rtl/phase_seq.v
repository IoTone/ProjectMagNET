// SPDX-License-Identifier: Apache-2.0
`timescale 1ns/1ps
//
// phase_seq -- 8-state half-step table for a ULN2003 + 28BYJ-48.
//
// One `step` pulse advances the index by one in `dir`; `phase` drives IN1..IN4.
// With `en` low every output is 0: the coils are de-energized, which is how the
// motor spends most of its life (it draws ~200 mA standing still otherwise, and
// the gearbox holds position without current anyway).
//
// BENCH NOTE: which way `dir`=1 turns depends on the coil wiring order. Check it
// once on the bench; swapping the middle two ULN2003 leads reverses it.
module phase_seq (
	input  wire       clk,
	input  wire       rst,
	input  wire       en,
	input  wire       step,
	input  wire       dir,	/* 1 = index up */
	output wire [3:0] phase
);
	reg [2:0] idx;

	always @(posedge clk) begin
		if (rst)
			idx <= 3'd0;
		else if (step)
			idx <= dir ? idx + 3'd1 : idx - 3'd1;
	end

	/* Half-step: one coil, then two, alternating. */
	reg [3:0] tbl;
	always @(*) begin
		case (idx)
			3'd0: tbl = 4'b1000;
			3'd1: tbl = 4'b1100;
			3'd2: tbl = 4'b0100;
			3'd3: tbl = 4'b0110;
			3'd4: tbl = 4'b0010;
			3'd5: tbl = 4'b0011;
			3'd6: tbl = 4'b0001;
			3'd7: tbl = 4'b1001;
		endcase
	end

	assign phase = en ? tbl : 4'b0000;
endmodule
