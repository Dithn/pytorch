import warnings

import torch.distributed as dist
import torch.distributed.algorithms.model_averaging.utils as utils


class PeriodicModelAverager:
    r"""
    Averages parameters periodically or during the warm-up stage.

    This can be used for running `post-local SDG <https://arxiv.org/abs/1808.07217>`_,
    by running :class:`~torch.nn.DistributedDataParallel` (DDP)
    using the subgroups created by :meth:`~torch.distributed.new_subgroups`.

    Args:
        module (torch.nn.Module): The module where its parameters will be averaged.
        period (int): The number of steps per model averaging.
                      Usually the period should be greater than ``1`` to reduce the communication cost.
                      Otherwise, only DDP needs to be used.
        warmup_steps (int): The number of warm-up steps. During this stage,
                            ``period`` is viewed as 1, and the parameters are averaged at every step.
        process_group: The process group to be used for all-reduce.
                       If ``None``, the default process group, which
                       is created by :func:`torch.distributed.init_process_group`,
                       will be used. (default: ``None``)

    Example::

        >>>  import torch
        >>>  import torch.distributed as dist
        >>>  import torch.distributed.algorithms.model_averaging.averagers as averagers
        >>>  import torch.nn as nn
        >>>
        >>>  dist.init_process_group("nccl", rank=rank, world_size=16)
        >>>  torch.cuda.set_device(rank)
        >>>  module = nn.Linear(1, 1, bias=False).to(rank)
        >>>  subgroup, subgroups = dist.new_subgroups()
        >>>  # Gradients are averaged by each intra-node subgroup during the backward pass.
        >>>  model = nn.parallel.DistributedDataParallel(
        >>>     module, device_ids=[rank], output_device=rank, process_group=subgroup
        >>>  )
        >>>
        >>>  # In the first 100 steps, run model averaging every step.
        >>>  # After 100 steps, run model averaging every 4 steps.
        >>>  averager = averagers.PeriodicModelAverager(model, warmup_steps=100, period=4)
        >>>  for step in range(0, 20):
        >>>     optimizer.zero_grad()
        >>>     loss = loss_fn(output, labels)
        >>>     loss.backward()
        >>>     optimizer.step()
        >>>     # Average parameters globally after ``optimizer.step()``.
        >>>     # Thus, the inter-node communication only occurs periodically after ``warmup_steps``.
        >>>     averager.average_parameters()

    .. warning ::
        `PeriodicModelAverager` is experimental and subject to change.
    """

    def __init__(
        self,
        module,
        period,
        warmup_steps=0,
        process_group=None,
    ):
        self.module = module
        if warmup_steps < 0:
            raise ValueError("Arg ``warmup_steps`` must be a non-negative number.")
        self.warmup_steps = warmup_steps
        if period < 1:
            raise ValueError("Arg ``period`` must be a positive value.")
        elif period == 1:
            warnings.warn(
                "When period is 1, no need to use model averaging because the communication cost "
                "of all-reducing parameters will be no less than the cost of all-reducing gradients "
                "by DistributedDataParall in the backward pass. Therefore, only "
                "DistributedDataParallel should be used for this case."
            )
        self.period = period
        self.process_group = (
            process_group if process_group is not None else dist.group.WORLD
        )
        self.step = 0

    def average_parameters(self):
        r"""
        Averages parameters if ``step`` is less than ``warmup_steps``,
        or it can be divided by ``period``, where ``step`` is increased by 1
        at each iteration in the training loop.
        """
        if self.step < self.warmup_steps or self.step % self.period == 0:
            utils.average_parameters(self.module, self.process_group)
        self.step += 1
