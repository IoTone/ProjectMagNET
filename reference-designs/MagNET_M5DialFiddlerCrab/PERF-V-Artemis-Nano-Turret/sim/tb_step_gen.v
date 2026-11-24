// SPDX-License-Identifier: Apache-2.0
//
// Does `vel` really mean steps per second, with no cumulative drift?
//
// The test that matters is the SECOND one. Counting over a single window is
// ambiguous by one step: `step` is registered a clock after the accumulator
// passes, and the window edges do not line up with that. Counting the steps
// BETWEEN two equally-phased instants removes the ambiguity entirely, and a
// rate with no cumulative error must deliver exactly v*(t2-t1) of them.
`timescale 1ns/1ps
module tb_step_gen;
	localparam CLK_HZ = 1_000_000;

	reg clk = 0, rst = 1, en = 0;
	reg [31:0] vel = 0;
	wire step;
	integer count, c1, c3, fails = 0;

	step_gen #(.CLK_HZ(CLK_HZ)) dut (
		.clk(clk), .rst(rst), .en(en), .vel(vel), .step(step)
	);

	always #500 clk = ~clk;		/* 1 MHz */

	always @(posedge clk) if (step) count = count + 1;

	task check_rate(input [31:0] v);
		begin
			vel = v; count = 0; en = 1;
			#1_000_000_000;  c1 = count;		/* t = 1 s */
			#2_000_000_000;  c3 = count;		/* t = 3 s */
			en = 0;

			/* Absolute count over the first second, within the one
			 * step of boundary slop. */
			if (c1 == v || c1 == v - 1)
				$display("PASS  vel=%0d -> %0d steps in the first second", v, c1);
			else begin
				$display("FAIL  vel=%0d -> %0d steps in the first second", v, c1);
				fails = fails + 1;
			end

			/* The real property: no drift between two like-phased marks. */
			if (c3 - c1 == 2 * v)
				$display("PASS  vel=%0d -> exactly %0d steps over seconds 2-3 (no drift)", v, c3 - c1);
			else begin
				$display("FAIL  vel=%0d -> %0d steps over seconds 2-3, want %0d",
				         v, c3 - c1, 2 * v);
				fails = fails + 1;
			end
			#10_000;
		end
	endtask

	initial begin
		count = 0;
		#2000 rst = 0;
		check_rate(1);
		check_rate(100);
		check_rate(997);	/* prime: catches any power-of-two fudge */
		check_rate(50_000);
		if (fails == 0) $display("tb_step_gen: ALL PASS");
		else            $display("tb_step_gen: %0d FAILURE(S)", fails);
		$finish;
	end
endmodule
