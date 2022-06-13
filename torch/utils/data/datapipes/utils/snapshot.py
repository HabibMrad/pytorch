from torch.utils.data.datapipes.datapipe import IterDataPipe


def simple_fast_forward(datapipe: IterDataPipe, n_iterations: int) -> None:
    r"""
    Simple fash-forward by skipping over `n` outputs and resume from there.
    """
    # Fast-forward only when the DP has recently been restored. Is this necessary?
    # if self._restored:
    remainder = n_iterations
    # self._seed = 0  # TODO: remove this once we figure out how _seed can persist
    print(f"Creating iterator for fast-forward of {datapipe}")
    it = iter(datapipe)
    print(f"About to fast-forward {datapipe}")
    while remainder > 0:
        try:
            next(it)
            remainder -= 1
        except StopIteration:
            raise RuntimeError(f"Fast-forward {datapipe} by {n_iterations} iterations"
                               "exceeds the number of samples available.")
    print(f"Fast-forward of {datapipe} has been completed")
    datapipe._fast_forward_iterator = it
    # This will prevent the DataPipe from resetting in the `iter()` call
    # If another DataPipe is consuming it, it won't have to start over again
    datapipe._restored = True


def fast_forward_graph(datapipe: IterDataPipe, n_iterations: int) -> None:

    # 1. Get a graph of the datapipe

    # 2. Identify the sources

    # 3. Traverse through the graph and fast-forward each, assuming the inputs have been fast-forwarded properly.

    # TODO: Think about what should happen if a DataPipe (perhaps custom by user) doesn't have a proper fast-forward
    #       function. Simple fast-forward could work (but you will need to skip fast-forwarding the
    #       DataPipes before).

    pass
