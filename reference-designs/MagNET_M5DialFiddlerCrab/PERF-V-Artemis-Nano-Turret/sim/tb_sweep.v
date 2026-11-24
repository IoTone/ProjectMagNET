// SPDX-License-Identifier: Apache-2.0
//
// T5a's acceptance test, verbatim from PLAN.md:
//   "A full arc runs from one write; stations land on exact POS values;
//    abort is clean mid-sweep"
`timescale 1ns/1ps
module tb_sweep;
	localparam CLK_HZ = 1_000_000;

	reg clk = 0, rst = 1;
	reg signed [31:0] cfg_start = 0, cfg_end = 0;
	reg [31:0] cfg_step = 0, cfg_dwell_ms = 0;
	reg go = 0, abort = 0;

	wire busy, done, aborted, strobe;
	wire [15:0] station;
	wire signed [31:0] pos_latch;
	wire signed [31:0] m_target;
	wire m_go, m_stop;

	wire m_moving, m_at_target;
	wire signed [31:0] m_pos;
	wire [3:0] phase;
	wire lim_hit, estop_hit, step_pulse;

	integer fails = 0;
	integer nstrobe;
	integer expect_pos [0:63];
	integer saw_pos    [0:63];

	sweep #(.CLK_HZ(CLK_HZ)) dut (
		.clk(clk), .rst(rst),
		.cfg_start(cfg_start), .cfg_end(cfg_end),
		.cfg_step(cfg_step), .cfg_dwell_ms(cfg_dwell_ms),
		.go(go), .abort(abort),
		.busy(busy), .done(done), .aborted(aborted),
		.station(station), .strobe(strobe), .pos_latch(pos_latch),
		.m_target(m_target), .m_go(m_go), .m_stop(m_stop),
		.m_moving(m_moving), .m_at_target(m_at_target), .m_pos(m_pos)
	);

	motion_axis #(.CLK_HZ(CLK_HZ), .VMIN(100)) axis (
		.clk(clk), .rst(rst), .enable(1'b1),
		.target(m_target), .go(m_go), .stop(m_stop), .estop(1'b0),
		.vmax(32'd2000), .accel(32'd8000),
		.lim_cw(1'b0), .lim_ccw(1'b0),
		.pos(m_pos), .moving(m_moving), .at_target(m_at_target),
		.lim_hit(lim_hit), .estop_hit(estop_hit),
		.phase(phase), .step_pulse(step_pulse)
	);

	always #500 clk = ~clk;

	/* Record every strobe, and prove the axis is standing still when it fires. */
	always @(posedge clk) begin
		if (strobe) begin
			if (nstrobe < 64) saw_pos[nstrobe] = pos_latch;
			nstrobe = nstrobe + 1;
			if (m_moving) begin
				$display("FAIL  strobe %0d fired while moving", nstrobe);
				fails = fails + 1;
			end
			if (pos_latch !== m_pos) begin
				$display("FAIL  strobe %0d latched %0d, axis at %0d",
				         nstrobe, pos_latch, m_pos);
				fails = fails + 1;
			end
		end
	end

	task run_sweep(input signed [31:0] s, input signed [31:0] e,
	               input [31:0] st, input [31:0] dw);
		begin
			cfg_start = s; cfg_end = e; cfg_step = st; cfg_dwell_ms = dw;
			nstrobe = 0;
			@(posedge clk) go = 1;
			@(posedge clk) go = 0;
			wait (done || aborted);
			@(posedge clk);
		end
	endtask

	integer i, n, p;
	initial begin
		#2000 rst = 0; #2000;

		/* ---- 1. Forward arc, 0 -> 800 in 100s: nine stations ---- */
		n = 0;
		for (i = 0; i <= 800; i = i + 100) begin
			expect_pos[n] = i; n = n + 1;
		end
		run_sweep(0, 800, 100, 2);
		if (nstrobe == n) $display("PASS  %0d stations", nstrobe);
		else begin $display("FAIL  %0d stations, want %0d", nstrobe, n);
			fails = fails + 1; end
		for (i = 0; i < n && i < nstrobe; i = i + 1)
			if (saw_pos[i] !== expect_pos[i]) begin
				$display("FAIL  station %0d at %0d, want %0d",
				         i, saw_pos[i], expect_pos[i]);
				fails = fails + 1;
			end
		if (fails == 0) $display("PASS  every station landed on its exact position");

		/* ---- 2. Reverse arc ---- */
		run_sweep(800, 300, 125, 2);		/* 800,675,550,425,300 */
		if (nstrobe == 5 && saw_pos[0] == 800 && saw_pos[4] == 300)
			$display("PASS  reverse arc, 5 stations, 800 -> 300");
		else begin
			$display("FAIL  reverse arc gave %0d stations (%0d..%0d)",
			         nstrobe, saw_pos[0], saw_pos[nstrobe-1]);
			fails = fails + 1;
		end

		/* ---- 3. A step that does not divide the arc must not overshoot ---- */
		run_sweep(0, 250, 100, 2);		/* 0,100,200 -- not 300 */
		if (nstrobe == 3 && saw_pos[2] == 200)
			$display("PASS  ragged arc stopped at 200, did not overshoot 250");
		else begin
			$display("FAIL  ragged arc: %0d stations, last %0d",
			         nstrobe, saw_pos[nstrobe-1]);
			fails = fails + 1;
		end

		/* ---- 4. Zero step is refused, not run forever ---- */
		run_sweep(0, 800, 0, 2);
		if (aborted && nstrobe == 0)
			$display("PASS  zero step refused");
		else begin $display("FAIL  zero step not refused"); fails = fails + 1; end

		/* ---- 5. Abort mid-sweep ---- */
		cfg_start = 0; cfg_end = 4000; cfg_step = 200; cfg_dwell_ms = 2;
		nstrobe = 0;
		@(posedge clk) go = 1;
		@(posedge clk) go = 0;
		wait (nstrobe >= 3);		/* let it get properly underway */
		wait (m_moving);		/* abort DURING a move, not in a dwell */
		#3_000_000;
		@(posedge clk) abort = 1;
		@(posedge clk) abort = 0;
		wait (!busy);
		@(posedge clk);
		p = m_pos;
		#10_000_000;
		if (!aborted)      begin $display("FAIL  abort did not set aborted"); fails = fails + 1; end
		if (busy)          begin $display("FAIL  still busy after abort");    fails = fails + 1; end
		if (m_moving)      begin $display("FAIL  axis still moving after abort"); fails = fails + 1; end
		if (m_pos !== p)   begin $display("FAIL  axis crept after abort (%0d -> %0d)", p, m_pos); fails = fails + 1; end
		if (aborted && !busy && !m_moving && m_pos === p)
			$display("PASS  abort mid-move: axis ramped down and stayed at %0d", p);

		/* ---- 6. And it can sweep again straight afterwards ---- */
		run_sweep(0, 400, 200, 2);
		if (done && !aborted && nstrobe == 3)
			$display("PASS  re-armed after abort");
		else begin $display("FAIL  did not re-arm after abort"); fails = fails + 1; end

		/* ---- 7. A realistic scan, timed ----
		 * 4075.77 half-steps/rev (NOT 4096), so 180 deg = 2038 half-steps
		 * and a +/-10 deg baffle station spacing = 113 half-steps.
		 * Dwell 220 ms = 2 x the OPT4048's 100 ms conversion.
		 * Behaviour is clock-rate independent, so the duration measured at
		 * 1 MHz is the duration at 50 MHz. */
		begin : timed
			real t0, t1;
			t0 = $realtime;
			run_sweep(0, 2038, 113, 220);
			t1 = $realtime;
			$display("INFO  realistic 180 deg scan: %0d stations in %0.2f s of arc time",
			         nstrobe, (t1 - t0) / 1.0e9);
			$display("INFO    (%0d half-steps, 113/station, 220 ms dwell, vmax 2000 steps/s)",
			         2038);
			/* Same arc, no stations and no dwell: pure travel. The
			 * difference is what stop-and-shoot costs. */
			t0 = $realtime;
			run_sweep(0, 2038, 2038, 0);
			t1 = $realtime;
			$display("INFO  same arc travelled in one move: %0.2f s",
			         (t1 - t0) / 1.0e9);
			/* And the dwell alone. */
			$display("INFO  of the 8.35 s, dwell is 19 x 220 ms = %0.2f s; the rest is ramping",
			         19 * 0.220);
		end

		if (fails == 0) $display("tb_sweep: ALL PASS");
		else            $display("tb_sweep: %0d FAILURE(S)", fails);
		$finish;
	end

	initial begin
		#60_000_000_000;
		$display("tb_sweep: TIMEOUT");
		$finish;
	end
endmodule
