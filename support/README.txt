Visit:

	 http://wiki.alliedmods.net/Category:Metamod:Source_Documentation 
	  
 for more information.


Interface diagnostics
---------------------

This feature is only present in binaries built with
`configure.py --enable-interface-trace`; it is compiled out by default.

Start the game or dedicated server with -interface-trace to record Metamod's
Source 2 interface queries, vtable slots, first calls, and hook installation.
The loader also accepts MMS_INTERFACE_TRACE=1 in the process environment.

The trace is written to metamod-interface.log beside the Metamod loader and
core binaries. Each loader/core session starts with a TRACE.START line. If a
crash leaves an ENTER line without the matching LEAVE line, that stage identifies
the interface call or hook operation that was active when the process stopped.


