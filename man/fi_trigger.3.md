---
layout: page
title: fi_trigger(3)
tagline: Libfabric Programmer's Manual
---
{% include JB/setup %}

# NAME

fi_trigger - Triggered operations

# SYNOPSIS

{% highlight c %}
#include <rdma/fi_trigger.h>
{% endhighlight %}

# DESCRIPTION

Triggered operations allow an application to queue a data transfer
request that is deferred until a specified condition is met.  A typical
use is to send a message only after receiving all input data.

A triggered operation may be requested by specifying the FI_TRIGGER
flag as part of the operation.  Alternatively, an endpoint alias may
be created and configured with the FI_TRIGGER flag.  Such an endpoint
is referred to as a trigger-able endpoint.  All data transfer
operations on a trigger-able endpoint are deferred.

Any data transfer operation is potentially trigger-able, subject to
provider constraints.  Trigger-able endpoints are initialized such that
only those interfaces supported by the provider which are trigger-able
are available.

Triggered operations require that applications use struct
fi_triggered_context as their per operation context parameter.  The
use of struct fi_triggered_context replaces struct fi_context, if
required by the provider.  Although struct fi_triggered_context is not
opaque to the application, the contents of the structure may be
modified by the provider.  This structure has similar requirements as
struct fi_context.  It must be allocated by the application and remain
valid until the corresponding operation completes or is successfully
canceled.

Struct fi_triggered_context is used to specify the condition that must
be met before the triggered data transfer is initiated.  If the
condition is met when the request is made, then the data transfer may
be initiated immediately.  The format of struct fi_triggered_context
is described below.

{% highlight c %}
struct fi_triggered_context {
	enum fi_trigger_event   event_type;   /* trigger type */
	union {
		struct fi_trigger_threshold	threshold;
		void                *internal[3]; /* reserved */
	} trigger;
};
{% endhighlight %}

The triggered context indicates the type of event assigned to the
trigger, along with a union of trigger details that is based on the
event type.

## TRIGGER EVENTS

The following trigger events are defined.

*FI_TRIGGER_THRESHOLD*
: This indicates that the data transfer operation will be deferred
  until an event counter crosses an application specified threshold
  value.  The threshold is specified using struct
  fi_trigger_threshold:

{% highlight c %}
struct fi_trigger_threshold {
	struct fid_cntr *cntr; /* event counter to check */
	size_t threshold;      /* threshold value */
};
{% endhighlight %}

Threshold operations are triggered in the order of the threshold
values.  This is true even if the counter increments by a value
greater than 1.  If two triggered operations have the same threshold,
they will be triggered in the order in which they were submitted to
the endpoint.

# SEE ALSO

[`fi_getinfo`(3)](fi_getinfo.3.html),
[`fi_endpoint`(3)](fi_endpoint.3.html),
[`fi_alias`(3)](fi_alias.3.html),
[`fi_cntr`(3)](fi_cntr.3.html)
